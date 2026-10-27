"""
DataFrame detection and conversion utilities.

Fully generic — no anofox-specific logic here.
"""

from __future__ import annotations

import uuid
from contextlib import contextmanager
from typing import Any, Iterator, Literal

from ._sql import finish, run


DFType = Literal["pandas", "polars"]


def detect_df_type(df: Any) -> DFType:
    """
    Return ``"pandas"`` or ``"polars"`` based on the type of *df*.

    Raises :class:`TypeError` if *df* is neither a pandas nor a polars
    DataFrame (or a subclass of one); a Series gets a ``to_frame()`` hint.
    """
    for cls in type(df).__mro__:
        library = cls.__module__.split(".")[0]
        if library in ("pandas", "polars"):
            if cls.__name__ == "DataFrame":
                return library  # type: ignore[return-value]
            if cls.__name__ == "Series":
                raise TypeError(
                    f"Expected a pandas or polars DataFrame, got a {library} Series; "
                    "pass series.to_frame() instead."
                )
    raise TypeError(
        f"Expected a pandas or polars DataFrame, got {type(df).__qualname__!r}."
    )


def is_arrow_table(obj: Any) -> bool:
    """True for a ``pyarrow.Table`` (or a subclass), which DuckDB can copy natively."""
    return any(
        cls.__module__.split(".")[0] == "pyarrow" and cls.__name__ == "Table"
        for cls in type(obj).__mro__
    )


def register_df_as_table(conn: Any, df: Any, name: str | None = None) -> str:
    """
    Materialise *df* as a real DuckDB table on *conn* and return the table name.

    Uses ``CREATE TABLE AS`` (not just ``register()``) so the table is visible
    to extension functions that open a fresh ``Connection(*context.db)``
    internally (e.g. isolation_forest, pii_scan_table).

    If *name* is *None* a random UUID-based name is generated.

    Callers are responsible for dropping the table via
    ``conn.execute(f"DROP TABLE IF EXISTS {name}")`` when done.
    """
    if name is None:
        name = f"_anofox_{uuid.uuid4().hex}"

    tmp_view = f"{name}_v"
    finish(conn)  # register() fails while a result of execute() is pending (DuckDB 2.0)
    conn.register(tmp_view, _registrable(df))
    run(conn, f'CREATE TABLE "{name}" AS SELECT * FROM "{tmp_view}"')
    conn.unregister(tmp_view)
    return name


def _registrable(df: Any) -> Any:
    """Return an object DuckDB can register for *df* (pandas goes through Arrow)."""
    if is_arrow_table(df):
        return df
    if detect_df_type(df) == "pandas":
        # Convert via PyArrow to handle pandas 3.x StringDtype and other
        # extended dtypes that DuckDB's register() may not recognise directly.
        # preserve_index=False: the index is not data; a non-range index would
        # otherwise become an extra "__index_level_0__" column.
        try:
            import pyarrow as pa
        except ImportError:
            return df
        try:
            return pa.Table.from_pandas(df, preserve_index=False)
        except (pa.ArrowTypeError, pa.ArrowInvalid) as exc:
            raise _mixed_type_error(df, exc) from exc
    return df


def _mixed_type_error(df: Any, exc: Exception) -> Exception:
    """
    Return a :class:`TypeError` naming the pandas column Arrow cannot convert.

    The usual cause is an ``object`` column that mixes value types, e.g. ``str``
    and ``int``; Arrow (and DuckDB) need one type per column.
    """
    import pyarrow as pa

    for column in df.columns:
        try:
            pa.array(df[column], from_pandas=True)
        except (pa.ArrowTypeError, pa.ArrowInvalid):
            types = sorted({type(v).__name__ for v in df[column].dropna()})
            return TypeError(
                f"Column {str(column)!r} mixes value types ({', '.join(types)}); "
                "a column needs one type to be copied into DuckDB. Convert it first, "
                f"e.g. df[{str(column)!r}] = df[{str(column)!r}].astype(\"string\") "
                "(keeps missing values as NULL)."
            )
    return TypeError(f"Cannot convert the DataFrame to Arrow: {exc}")


def with_position(df: Any, name: str) -> Any:
    """Return a copy of *df* with a 0-based row position column *name*."""
    positions = range(len(df))
    if detect_df_type(df) == "pandas":
        return df.assign(**{name: list(positions)})
    import polars as pl
    return df.with_columns(pl.Series(name, list(positions), dtype=pl.Int64))


@contextmanager
def registered_view(conn: Any, df: Any) -> Iterator[tuple[str, str]]:
    """
    Register *df* plus a row position column as a temporary view on *conn*.

    Yields ``(view_name, position_column)``; the view is removed on exit. The
    position column has a random name, so it never replaces a column of *df*.
    Unlike :func:`register_df_as_table` nothing is copied, so this suits scalar
    functions, not table functions that read through a second connection.
    """
    suffix = uuid.uuid4().hex
    name = f"_anofox_v_{suffix}"
    position_column = f"__anofox_pos_{suffix}"
    finish(conn)  # register() fails while a result of execute() is pending (DuckDB 2.0)
    conn.register(name, _registrable(with_position(df, position_column)))
    try:
        yield name, position_column
    finally:
        try:
            finish(conn)
            conn.unregister(name)
        except Exception:
            pass


def drop_df_table(conn: Any, name: str) -> None:
    """Drop a table previously created by :func:`register_df_as_table`."""
    try:
        run(conn, f'DROP TABLE IF EXISTS "{name}"')
    except Exception:
        pass


def relation_to_dataframe(rel: Any, df_type: DFType) -> Any:
    """
    Convert a DuckDB relation/result to the requested DataFrame type.

    Parameters
    ----------
    rel:
        A DuckDB ``DuckDBPyRelation`` (from ``conn.sql(...)``). The result of
        ``conn.execute(...)`` has no ``.pl()`` on DuckDB 2.0.
    df_type:
        ``"pandas"`` or ``"polars"``.
    """
    if df_type == "pandas":
        return rel.df()
    # polars
    return rel.pl()
