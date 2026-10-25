"""
Shared table lifecycle helpers for wrappers that accept table names or DataFrames.
"""

from __future__ import annotations

from typing import Any, Union

from ._sql import quote_ident
from ._utils import detect_df_type, drop_df_table, register_df_as_table, relation_to_dataframe


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


def cleanup_table(conn: Any, table_name: str, was_registered: bool) -> None:
    """Drop a table only when it was created by :func:`resolve_table`."""
    if was_registered:
        drop_df_table(conn.native, table_name)


def apply_scalar(
    conn: Any,
    df: Any,
    df_type: str,
    function: str,
    column: str,
    extra_args: list,
    result_name: str,
) -> Any:
    """
    Return *df* plus a *result_name* column computed by the scalar SQL
    ``function(column, *extra_args)``; *extra_args* are bound as parameters.
    """
    table_name = register_df_as_table(conn.native, df)
    try:
        placeholders = "".join(", ?" for _ in extra_args)
        rel = conn.native.execute(
            f"SELECT *, {function}({quote_ident(column)}{placeholders}) AS {quote_ident(result_name)} "
            f"FROM {quote_ident(table_name)}",
            extra_args,
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)
