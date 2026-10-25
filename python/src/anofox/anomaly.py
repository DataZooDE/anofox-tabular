"""
Anomaly detection wrappers (Isolation Forest, DBSCAN, Outlier Tree).

Functions accept either a table name string or a DataFrame.
When ``output_mode="summary"`` they return a ``dict``; when
``output_mode="scores"`` they return a DataFrame: polars for a polars input,
pandas otherwise.
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._sql import fetch_frame, id_column_arg
from ._table import cleanup_table, output_df_type, resolve_table


def isolation_forest(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    n_trees: int = 100,
    sample_size: Optional[int] = None,
    contamination: float = 0.1,
    id_column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Run Isolation Forest on a single numeric column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    column:
        Numeric column name to analyse.
    output_mode:
        ``"summary"`` → dict with counts; ``"scores"`` → DataFrame with
        anomaly scores per row.
    n_trees:
        Number of isolation trees (default 100).
    sample_size:
        Sub-sample size per tree.  When ``None`` the extension default is used.
    contamination:
        Expected fraction of anomalies (default 0.1).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``.

    Returns
    -------
    dict or DataFrame
    """
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        actual_sample_size = sample_size if sample_size is not None else 256
        params = [table_name, column, n_trees, actual_sample_size, contamination, output_mode]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest(?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def isolation_forest_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: list[str],
    output_mode: str = "summary",
    n_trees: int = 100,
    sample_size: Optional[int] = None,
    contamination: float = 0.1,
    id_column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Run multivariate Isolation Forest across multiple numeric columns.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    columns:
        List of numeric column names to include.
    output_mode:
        ``"summary"`` → dict; ``"scores"`` → DataFrame.
    n_trees:
        Number of isolation trees (default 100).
    sample_size:
        Sub-sample size per tree.
    contamination:
        Expected fraction of anomalies (default 0.1).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    cols_csv = ",".join(columns)
    try:
        actual_sample_size = sample_size if sample_size is not None else 256
        params = [table_name, cols_csv, n_trees, actual_sample_size, contamination, output_mode]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest_mv(?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def dbscan(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    epsilon: float = 0.5,
    min_points: int = 5,
    id_column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Run DBSCAN clustering on a single numeric column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    column:
        Numeric column name.
    output_mode:
        ``"summary"`` → dict; ``"scores"`` (or ``"clusters"``) → DataFrame
        with one row per input row.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    # The extension's per-row DBSCAN mode is named 'clusters'; keep the
    # package-level "scores" naming consistent with the other detectors.
    sql_mode = "clusters" if output_mode == "scores" else output_mode
    try:
        params = [table_name, column, epsilon, min_points, sql_mode]
        query = (
            "SELECT * FROM anofox_tab_dbscan(?, ?, ?::DOUBLE, ?::BIGINT, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def dbscan_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: list[str],
    output_mode: str = "summary",
    epsilon: float = 0.5,
    min_points: int = 5,
    id_column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Run multivariate DBSCAN across multiple numeric columns.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    columns:
        List of numeric column names.
    output_mode:
        ``"summary"`` → dict; ``"scores"`` (or ``"clusters"``) → DataFrame
        with one row per input row.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    table_name, registered = resolve_table(conn, table_or_df)
    cols_csv = ",".join(columns)
    sql_mode = "clusters" if output_mode == "scores" else output_mode
    try:
        params = [table_name, cols_csv, epsilon, min_points, sql_mode]
        query = (
            "SELECT * FROM anofox_tab_dbscan_mv(?, ?, ?::DOUBLE, ?::BIGINT, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def outlier_tree(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, list[str]],
    output_mode: str = "summary",
    max_depth: int = 4,
    max_perc_outliers: float = 0.1,
    min_size_numeric: int = 10,
    min_size_categ: int = 10,
    z_norm: float = 2.67,
    z_outlier: float = 8.0,
    id_column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Run Outlier Tree on one or more columns (explainable anomaly detection).

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_or_df:
        Table name string or a pandas/polars DataFrame.
    columns:
        Column name or list of column names to analyse.
    output_mode:
        ``"summary"`` → dict with counts; ``"outliers"`` → DataFrame with
        per-row explanations.
    max_depth:
        Maximum tree depth (default 4).
    max_perc_outliers:
        Maximum fraction of outliers allowed (default 0.1).
    min_size_numeric:
        Minimum cluster size for numeric columns (default 10).
    min_size_categ:
        Minimum cluster size for categorical columns (default 10).
    z_norm:
        Z-score threshold for normal data (default 2.67).
    z_outlier:
        Z-score threshold for outliers (default 8.0).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="outliers"``.

    Returns
    -------
    dict or DataFrame
    """
    table_name, registered = resolve_table(conn, table_or_df)
    if isinstance(columns, list):
        cols_csv = ",".join(columns)
    else:
        cols_csv = columns
    try:
        params = [table_name, cols_csv, output_mode, max_depth, max_perc_outliers,
                  min_size_numeric, min_size_categ, z_norm, z_outlier]
        query = (
            "SELECT * FROM anofox_tab_outlier_tree(?, ?, ?, ?::INTEGER, ?::DOUBLE, "
            "?::INTEGER, ?::INTEGER, ?::DOUBLE, ?::DOUBLE"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def _execute_anomaly(
    conn: Any, query: str, params: list, output_mode: str, df_type: str
) -> Union[dict, Any]:
    """Execute the query and return either a dict or a DataFrame of *df_type*."""
    if output_mode in ("scores", "outliers"):
        return fetch_frame(conn, query, params, df_type)

    # summary — convert first row to dict using column names from description
    cursor = conn.execute(query, params)
    col_names = [d[0] for d in cursor.description]
    rows = cursor.fetchall()

    if not rows:
        return {"status": "error", "error": "no result"}

    return dict(zip(col_names, rows[0]))
