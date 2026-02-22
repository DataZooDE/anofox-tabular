"""
Shared table lifecycle helpers for wrappers that accept table names or DataFrames.
"""

from __future__ import annotations

from typing import Any, Union

from ._utils import detect_df_type, drop_df_table, register_df_as_table


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
