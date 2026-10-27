"""
Data profiling wrappers for anofox-tabular.

Functions accept either a table name string or a DataFrame (pandas/polars).
All functions return a DataFrame of the input's library (polars for a polars
DataFrame, else pandas).
"""

from __future__ import annotations

from typing import Any, Optional, Sequence, Union

from ._sql import fetch_frame
from ._table import DataFrame, column_list, output_df_type, table_inputs


def profile_table(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, Sequence[str], None] = None,
    sample_size: int = 1_000_000,
    exact: bool = False,
) -> DataFrame:
    """
    Per-column statistics — one row per column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    columns:
        Optional list of column names to restrict profiling to; a single name
        (``"email"``) profiles that one column.
    sample_size:
        Maximum rows to sample (default 1,000,000).  When ``exact=False``
        and the table has more rows, a random sample is taken.
    exact:
        When ``True``, always scan the full table.

    Returns
    -------
    DataFrame (pandas, or polars for a polars input)
        29 columns: column_name, column_type, row_count, null_count,
        null_rate, distinct_count, distinct_rate, min_val, max_val,
        mean, median, stddev, p25, p75, skewness, kurtosis, top_values,
        avg_length, min_length, max_length, pattern_summary, is_unique,
        is_constant, zero_count, negative_count, is_sampled,
        actual_sample_size, nan_count, inf_count. The numeric statistics
        use the finite values only; NaN and Inf values are counted in
        nan_count / inf_count.
    """
    with table_inputs(conn, table_or_df) as (table_name,):
        return fetch_frame(
            conn,
            "SELECT * FROM anofox_tab_profile_table(?, ?::VARCHAR[], ?::BIGINT, ?::BOOLEAN)",
            [table_name, column_list(columns) or [], sample_size, bool(exact)],
            output_df_type(table_or_df),
        )


def profile_summary(
    conn: Any,
    table_or_df: Union[str, Any],
) -> DataFrame:
    """
    Single-row table overview.

    Returns
    -------
    DataFrame (pandas, or polars for a polars input)
        One row with 12 columns: row_count, column_count, numeric_columns,
        string_columns, temporal_columns, boolean_columns,
        complex_columns, other_columns, total_nulls, total_null_rate,
        duplicate_row_count, estimated_memory_bytes (a rough estimate:
        row_count * column_count * 8).
    """
    with table_inputs(conn, table_or_df) as (table_name,):
        return fetch_frame(
            conn, "SELECT * FROM anofox_tab_profile_summary(?)", [table_name],
            output_df_type(table_or_df),
        )


def profile_correlations(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, Sequence[str], None] = None,
) -> DataFrame:
    """
    Pairwise Pearson and Spearman correlations for numeric columns.

    *columns* restricts the pairs to these columns (a list; a single name means
    that one column); ``None`` uses every numeric column.

    Returns
    -------
    DataFrame (pandas, or polars for a polars input)
        Columns: column_a, column_b, pearson, spearman, n.
    """
    with table_inputs(conn, table_or_df) as (table_name,):
        return fetch_frame(
            conn,
            "SELECT * FROM anofox_tab_profile_correlations(?, ?::VARCHAR[])",
            [table_name, column_list(columns) or []],
            output_df_type(table_or_df),
        )
