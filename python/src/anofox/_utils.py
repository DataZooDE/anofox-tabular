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
    DataFrame.
    """
    module = type(df).__module__.split(".")[0]
    if module == "pandas":
        return "pandas"
    if module == "polars":
        return "polars"
    raise TypeError(
        f"Expected a pandas or polars DataFrame, got {type(df).__qualname__!r}."
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
    if detect_df_type(df) == "pandas":
        # Convert via PyArrow to handle pandas 3.x StringDtype and other
        # extended dtypes that DuckDB's register() may not recognise directly.
        # preserve_index=False: the index is not data; a non-range index would
        # otherwise become an extra "__index_level_0__" column.
        try:
            import pyarrow as pa
            return pa.Table.from_pandas(df, preserve_index=False)
        except ImportError:
            return df
    return df


POSITION_COLUMN = "__anofox_pos"


def with_position(df: Any) -> Any:
    """Return a copy of *df* with a 0-based row position column :data:`POSITION_COLUMN`."""
    positions = range(len(df))
    if detect_df_type(df) == "pandas":
        return df.assign(**{POSITION_COLUMN: list(positions)})
    import polars as pl
    return df.with_columns(pl.Series(POSITION_COLUMN, list(positions), dtype=pl.Int64))


@contextmanager
def registered_view(conn: Any, df: Any) -> Iterator[str]:
    """
    Register *df* plus a :data:`POSITION_COLUMN` as a temporary view on *conn*.

    Yields the view name; the view is removed on exit. Unlike
    :func:`register_df_as_table` nothing is copied, so this suits scalar
    functions, not table functions that read through a second connection.
    """
    name = f"_anofox_v_{uuid.uuid4().hex}"
    finish(conn)  # register() fails while a result of execute() is pending (DuckDB 2.0)
    conn.register(name, _registrable(with_position(df)))
    try:
        yield name
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
