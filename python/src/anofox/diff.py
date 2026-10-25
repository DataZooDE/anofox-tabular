"""
Data diff wrappers (join-based and hash-based).
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._sql import fetch_frame
from ._table import cleanup_table, output_df_type, resolve_table


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
    DataFrame (pandas, or polars when a polars DataFrame is passed)
        Rows with ``diff_type`` column indicating change type.
    """
    src_name, src_registered = resolve_table(conn, src)
    tgt_name, tgt_registered = resolve_table(conn, tgt)
    pk_sql, pk_value = _format_pk(primary_keys)

    try:
        params: list = [src_name, tgt_name, pk_value]
        query = f"SELECT * FROM anofox_tab_diff_joindiff(?, ?, {pk_sql}"
        if compare_columns is not None:
            query += ", ?::VARCHAR[]"
            params.append(list(compare_columns))
            if include_unchanged:
                query += ", true"
        query += ")"

        result = fetch_frame(conn, query, params, output_df_type(src, tgt))
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
    DataFrame (pandas, or polars when a polars DataFrame is passed)
        Rows where hashes differ, with ``diff_type`` column.
    """
    src_name, src_registered = resolve_table(conn, src)
    tgt_name, tgt_registered = resolve_table(conn, tgt)
    pk_sql, pk_value = _format_pk(primary_keys)

    try:
        params: list = [src_name, tgt_name, pk_value]
        query = f"SELECT * FROM anofox_tab_diff_hashdiff(?, ?, {pk_sql}"
        if bisection_threshold is not None:
            query += ", ?::BIGINT"
            params.append(bisection_threshold)
            if bisection_factor is not None:
                query += ", ?::BIGINT"
                params.append(bisection_factor)
        query += ")"

        result = fetch_frame(conn, query, params, output_df_type(src, tgt))
    finally:
        cleanup_table(conn, src_name, src_registered)
        cleanup_table(conn, tgt_name, tgt_registered)
    return result


def _format_pk(primary_keys: Union[str, list[str]]) -> tuple[str, Any]:
    """Return the placeholder SQL and the bound value of the primary key argument."""
    if isinstance(primary_keys, str):
        return "?", primary_keys
    if len(primary_keys) == 1:
        return "?", primary_keys[0]
    # Compound key — pass as a LIST<VARCHAR>
    return "?::VARCHAR[]", list(primary_keys)
