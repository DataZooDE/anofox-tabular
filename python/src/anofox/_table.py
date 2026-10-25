"""
Shared table lifecycle helpers for wrappers that accept table names or DataFrames.
"""

from __future__ import annotations

from typing import Any, Union

from ._sql import quote_ident
from ._utils import (
    POSITION_COLUMN,
    detect_df_type,
    drop_df_table,
    register_df_as_table,
    registered_view,
)


def resolve_table(conn: Any, table_or_df: Union[str, Any]) -> tuple[str, bool]:
    """
    Return ``(table_name, was_registered)``.

    If *table_or_df* is a DataFrame, it is materialized as a temporary table.
    """
    if isinstance(table_or_df, str):
        return table_or_df, False
    detect_df_type(table_or_df)  # validate type before registering
    table_name = register_df_as_table(conn.native, table_or_df)
    return table_name, True


def output_df_type(*inputs: Any) -> str:
    """
    DataFrame library for a result: ``"polars"`` when the first DataFrame among
    *inputs* is a polars DataFrame, else ``"pandas"`` (also for table names).
    """
    for value in inputs:
        if not isinstance(value, str):
            return detect_df_type(value)
    return "pandas"


def cleanup_table(conn: Any, table_name: str, was_registered: bool) -> None:
    """Drop a table only when it was created by :func:`resolve_table`."""
    if was_registered:
        drop_df_table(conn.native, table_name)


def apply_scalar(
    conn: Any,
    df: Any,
    df_type: str,
    expression: str,
    params: list,
    result_name: str,
) -> Any:
    """
    Return a copy of *df* with one added column *result_name*.

    *expression* is a SQL expression over the columns of *df* (identifiers
    already quoted) with ``?`` placeholders for *params*. The result keeps the
    input's rows in their order, its other columns unchanged and, for pandas,
    its index, so the new column can be assigned back to the input.
    """
    with registered_view(conn.native, df) as view:
        result = conn.native.execute(
            f"SELECT {expression} AS {quote_ident(result_name)} "
            f"FROM {quote_ident(view)} ORDER BY {quote_ident(POSITION_COLUMN)}",
            params,
        )
        if df_type == "pandas":
            values = result.df()[result_name]
            values.index = df.index
            out = df.copy()
            out[result_name] = values
            return out
        values = result.pl().get_column(result_name)
        return df.with_columns(values)
