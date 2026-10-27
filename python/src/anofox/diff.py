"""
Data diff wrappers (join-based and hash-based row diffs, schema diff).
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._sql import fetch_frame
from ._table import output_df_type, table_inputs


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
        Rows with ``diff_type`` column indicating change type and a last
        ``changed_columns`` column listing the differing columns of each
        changed row (``NULL`` for added and removed rows).
    """
    pk_sql, pk_value = _format_pk(primary_keys)
    labels = ["the source DataFrame", "the target DataFrame"]
    with table_inputs(conn, src, tgt, labels=labels) as (src_name, tgt_name):
        params: list = [src_name, tgt_name, pk_value]
        query = f"SELECT * FROM anofox_tab_diff_joindiff(?, ?, {pk_sql}"
        if compare_columns is not None:
            query += ", ?::VARCHAR[]"
            params.append(list(compare_columns))
            if include_unchanged:
                query += ", true"
        query += ")"

        result = fetch_frame(conn, query, params, output_df_type(src, tgt))
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
        Rows where hashes differ, with ``diff_type`` column and a last
        ``changed_columns`` column (as for :func:`joindiff`).
    """
    pk_sql, pk_value = _format_pk(primary_keys)
    labels = ["the source DataFrame", "the target DataFrame"]
    with table_inputs(conn, src, tgt, labels=labels) as (src_name, tgt_name):
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
    return result


def diff_schema(
    conn: Any,
    source: Union[str, Any],
    target: Union[str, Any],
) -> Any:
    """
    Compare the columns of two tables.

    Returns one row per column of either table with its type on each side and
    the drift: ``same``, ``added`` (target only), ``removed`` (source only) or
    ``retyped``.  Columns match case-insensitively; the rows come in source
    column order, followed by the added columns in target column order.  Run
    it before :func:`joindiff` / :func:`hashdiff` to see which columns those
    compare.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    source:
        Old table name (tables, views, TEMP tables, ``schema.table``) or
        DataFrame.
    target:
        New table name or DataFrame.

    Returns
    -------
    DataFrame (pandas, or polars when a polars DataFrame is passed)
        Columns ``column_name``, ``source_type``, ``target_type`` (missing,
        i.e. ``NaN`` in pandas and ``null`` in polars, on the side that lacks
        the column) and ``drift``.
    """
    labels = ["the source DataFrame", "the target DataFrame"]
    with table_inputs(conn, source, target, labels=labels) as (src_name, tgt_name):
        result = fetch_frame(
            conn,
            "SELECT * FROM anofox_tab_diff_schema(?, ?)",
            [src_name, tgt_name],
            output_df_type(source, target),
        )
    return result


def _format_pk(primary_keys: Union[str, list[str]]) -> tuple[str, Any]:
    """Return the placeholder SQL and the bound value of the primary key argument."""
    if isinstance(primary_keys, str):
        return "?", primary_keys
    if len(primary_keys) == 1:
        return "?", primary_keys[0]
    # Compound key — pass as a LIST<VARCHAR>
    return "?::VARCHAR[]", list(primary_keys)
