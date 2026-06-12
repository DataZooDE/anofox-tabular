"""
Data diff wrappers (join-based and hash-based).
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._table import cleanup_table, resolve_table


def joindiff(
    conn: Any,
    src: Union[str, Any],
    tgt: Union[str, Any],
    primary_keys: Union[str, list[str]],
    compare_columns: Optional[list[str]] = None,
    include_unchanged: bool = False,
) -> Any:
    """
    Compare two tables using a full-outer-join strategy.

    Detects added, removed, changed, and (optionally) unchanged rows.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    src:
        Source table name or DataFrame.
    tgt:
        Target table name or DataFrame.
    primary_keys:
        Column name(s) forming the primary key.  Either a string (single
        key) or a list of strings (compound key).
    compare_columns:
        Columns to compare.  When *None* all non-key columns are compared.
    include_unchanged:
        Include unchanged rows in the result (default False).

    Returns
    -------
    pd.DataFrame
        Rows with ``diff_type`` column indicating change type.
    """
    src_name, src_registered = resolve_table(conn, src)
    tgt_name, tgt_registered = resolve_table(conn, tgt)
    pk_arg = _format_pk(primary_keys)

    try:
        if compare_columns is not None and include_unchanged:
            cols_list = "[" + ", ".join(f"'{c}'" for c in compare_columns) + "]"
            query = (
                f"SELECT * FROM anofox_tab_diff_joindiff("
                f"'{src_name}', '{tgt_name}', {pk_arg}, {cols_list}, true)"
            )
        elif compare_columns is not None:
            cols_list = "[" + ", ".join(f"'{c}'" for c in compare_columns) + "]"
            query = (
                f"SELECT * FROM anofox_tab_diff_joindiff("
                f"'{src_name}', '{tgt_name}', {pk_arg}, {cols_list})"
            )
        else:
            query = f"SELECT * FROM anofox_tab_diff_joindiff('{src_name}', '{tgt_name}', {pk_arg})"

        result = conn.native.sql(query).df()
    finally:
        cleanup_table(conn, src_name, src_registered)
        cleanup_table(conn, tgt_name, tgt_registered)
    return result


def hashdiff(
    conn: Any,
    src: Union[str, Any],
    tgt: Union[str, Any],
    primary_keys: Union[str, list[str]],
    bisection_threshold: Optional[int] = None,
    bisection_factor: Optional[int] = None,
) -> Any:
    """
    Compare two tables using row hashes.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    src:
        Source table name or DataFrame.
    tgt:
        Target table name or DataFrame.
    primary_keys:
        Column name(s) forming the primary key.
    bisection_threshold:
        Row-count threshold for bisection. Not implemented yet — passing a
        value raises a DuckDB binder error.
    bisection_factor:
        Bisection factor (requires bisection_threshold). Not implemented
        yet — passing a value raises a DuckDB binder error.

    Returns
    -------
    pd.DataFrame
        Rows where hashes differ, with ``diff_type`` column.
    """
    src_name, src_registered = resolve_table(conn, src)
    tgt_name, tgt_registered = resolve_table(conn, tgt)
    pk_arg = _format_pk(primary_keys)

    try:
        if bisection_threshold is not None and bisection_factor is not None:
            query = (
                f"SELECT * FROM anofox_tab_diff_hashdiff("
                f"'{src_name}', '{tgt_name}', {pk_arg}, {bisection_threshold}, {bisection_factor})"
            )
        elif bisection_threshold is not None:
            query = (
                f"SELECT * FROM anofox_tab_diff_hashdiff("
                f"'{src_name}', '{tgt_name}', {pk_arg}, {bisection_threshold})"
            )
        else:
            query = f"SELECT * FROM anofox_tab_diff_hashdiff('{src_name}', '{tgt_name}', {pk_arg})"

        result = conn.native.sql(query).df()
    finally:
        cleanup_table(conn, src_name, src_registered)
        cleanup_table(conn, tgt_name, tgt_registered)
    return result


def _format_pk(primary_keys: Union[str, list[str]]) -> str:
    """Format primary key argument for SQL."""
    if isinstance(primary_keys, str):
        return f"'{primary_keys}'"
    if len(primary_keys) == 1:
        return f"'{primary_keys[0]}'"
    # Compound key — pass as a LIST<VARCHAR> literal
    return "[" + ", ".join(f"'{k}'" for k in primary_keys) + "]"
