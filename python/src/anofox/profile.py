"""
Data profiling wrappers for anofox-tabular.

Functions accept either a table name string or a DataFrame (pandas/polars).
All functions return a pandas DataFrame.
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._table import cleanup_table, resolve_table


def _escape_sql_literal(value: str) -> str:
    return value.replace("'", "''")


def profile_table(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Optional[list[str]] = None,
    sample_size: int = 1_000_000,
    exact: bool = False,
) -> Any:
    """
    Per-column statistics — one row per column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    columns:
        Optional list of column names to restrict profiling to.
    sample_size:
        Maximum rows to sample (default 1,000,000).  When ``exact=False``
        and the table has more rows, a random sample is taken.
    exact:
        When ``True``, always scan the full table.

    Returns
    -------
    pandas.DataFrame
        27 columns: column_name, column_type, row_count, null_count,
        null_rate, distinct_count, distinct_rate, min_val, max_val,
        mean, median, stddev, p25, p75, skewness, kurtosis, top_values,
        avg_length, min_length, max_length, pattern_summary, is_unique,
        is_constant, zero_count, negative_count, is_sampled,
        actual_sample_size.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        table_name_sql = _escape_sql_literal(table_name)
        cols_sql = "[]::VARCHAR[]"
        if columns:
            cols_sql = "[" + ", ".join(f"'{_escape_sql_literal(c)}'" for c in columns) + "]"
        query = (
            f"SELECT * FROM anofox_tab_profile_table("
            f"'{table_name_sql}', {cols_sql}, {sample_size}, {str(exact).lower()})"
        )
        return conn.native.sql(query).df()
    finally:
        cleanup_table(conn, table_name, registered)


def profile_summary(
    conn: Any,
    table_or_df: Union[str, Any],
) -> Any:
    """
    Single-row table overview.

    Returns
    -------
    pandas.DataFrame
        One row with: row_count, column_count, numeric_columns,
        string_columns, temporal_columns, boolean_columns,
        complex_columns, total_nulls, total_null_rate,
        duplicate_row_count.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        table_name_sql = _escape_sql_literal(table_name)
        return conn.native.sql(
            f"SELECT * FROM anofox_tab_profile_summary('{table_name_sql}')"
        ).df()
    finally:
        cleanup_table(conn, table_name, registered)


def profile_correlations(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Optional[list[str]] = None,
) -> Any:
    """
    Pairwise Pearson and Spearman correlations for numeric columns.

    Returns
    -------
    pandas.DataFrame
        Columns: column_a, column_b, pearson, spearman, n.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        table_name_sql = _escape_sql_literal(table_name)
        cols_sql = "[]::VARCHAR[]"
        if columns:
            cols_sql = "[" + ", ".join(f"'{_escape_sql_literal(c)}'" for c in columns) + "]"
        return conn.native.sql(
            f"SELECT * FROM anofox_tab_profile_correlations('{table_name_sql}', {cols_sql})"
        ).df()
    finally:
        cleanup_table(conn, table_name, registered)
