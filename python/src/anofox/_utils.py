"""
DataFrame detection and conversion utilities.

Fully generic — no anofox-specific logic here.
"""

from __future__ import annotations

import uuid
from typing import Any, Literal


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
    df_type = detect_df_type(df)
    if df_type == "pandas":
        # Convert via PyArrow to handle pandas 3.x StringDtype and other
        # extended dtypes that DuckDB's register() may not recognise directly.
        try:
            import pyarrow as pa
            arrow_table = pa.Table.from_pandas(df)
            conn.register(tmp_view, arrow_table)
        except ImportError:
            conn.register(tmp_view, df)
    else:
        conn.register(tmp_view, df)

    conn.execute(f'CREATE TABLE "{name}" AS SELECT * FROM "{tmp_view}"')
    conn.unregister(tmp_view)
    return name


def drop_df_table(conn: Any, name: str) -> None:
    """Drop a table previously created by :func:`register_df_as_table`."""
    try:
        conn.execute(f'DROP TABLE IF EXISTS "{name}"')
    except Exception:
        pass


def relation_to_dataframe(rel: Any, df_type: DFType) -> Any:
    """
    Convert a DuckDB relation/result to the requested DataFrame type.

    Parameters
    ----------
    rel:
        A DuckDB ``DuckDBPyRelation`` or result object with ``.df()`` /
        ``.pl()`` methods.
    df_type:
        ``"pandas"`` or ``"polars"``.
    """
    if df_type == "pandas":
        return rel.df()
    # polars
    return rel.pl()
