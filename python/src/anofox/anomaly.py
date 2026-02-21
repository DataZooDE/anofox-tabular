"""
Anomaly detection wrappers (Isolation Forest, DBSCAN, Outlier Tree).

Functions accept either a table name string or a DataFrame.
When ``output_mode="summary"`` they return a ``dict``; when
``output_mode="scores"`` they return a pandas DataFrame.
"""

from __future__ import annotations

from typing import Any, Optional, Union

from ._utils import detect_df_type, drop_df_table, register_df_as_table


def isolation_forest(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    n_trees: int = 100,
    sample_size: Optional[int] = None,
    contamination: float = 0.1,
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

    Returns
    -------
    dict or DataFrame
    """
    table_name, registered = _resolve_table(conn, table_or_df)
    try:
        actual_sample_size = sample_size if sample_size is not None else 256
        query = (
            f"SELECT * FROM anofox_tab_isolation_forest("
            f"'{table_name}', '{column}', {n_trees}, {actual_sample_size}, {contamination}, '{output_mode}')"
        )
        return _execute_anomaly(conn, query, output_mode)
    finally:
        _maybe_unregister(conn, table_name, registered)


def isolation_forest_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: list[str],
    output_mode: str = "summary",
    n_trees: int = 100,
    sample_size: Optional[int] = None,
    contamination: float = 0.1,
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
    """
    table_name, registered = _resolve_table(conn, table_or_df)
    cols_csv = ",".join(columns)
    try:
        actual_sample_size = sample_size if sample_size is not None else 256
        query = (
            f"SELECT * FROM anofox_tab_isolation_forest_mv("
            f"'{table_name}', '{cols_csv}', {n_trees}, {actual_sample_size}, {contamination}, '{output_mode}')"
        )
        return _execute_anomaly(conn, query, output_mode)
    finally:
        _maybe_unregister(conn, table_name, registered)


def dbscan(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    epsilon: float = 0.5,
    min_points: int = 5,
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
        ``"summary"`` → dict; ``"scores"`` → DataFrame.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    """
    table_name, registered = _resolve_table(conn, table_or_df)
    try:
        query = (
            f"SELECT * FROM anofox_tab_dbscan("
            f"'{table_name}', '{column}', {epsilon}, {min_points}, '{output_mode}')"
        )
        return _execute_anomaly(conn, query, output_mode)
    finally:
        _maybe_unregister(conn, table_name, registered)


def dbscan_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: list[str],
    output_mode: str = "summary",
    epsilon: float = 0.5,
    min_points: int = 5,
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
        ``"summary"`` → dict; ``"scores"`` → DataFrame.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    """
    table_name, registered = _resolve_table(conn, table_or_df)
    cols_csv = ",".join(columns)
    try:
        query = (
            f"SELECT * FROM anofox_tab_dbscan_mv("
            f"'{table_name}', '{cols_csv}', {epsilon}, {min_points}, '{output_mode}')"
        )
        return _execute_anomaly(conn, query, output_mode)
    finally:
        _maybe_unregister(conn, table_name, registered)


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

    Returns
    -------
    dict or DataFrame
    """
    table_name, registered = _resolve_table(conn, table_or_df)
    if isinstance(columns, list):
        cols_csv = ",".join(columns)
    else:
        cols_csv = columns
    try:
        query = (
            f"SELECT * FROM anofox_tab_outlier_tree("
            f"'{table_name}', '{cols_csv}', '{output_mode}', "
            f"{max_depth}, {max_perc_outliers}, {min_size_numeric}, "
            f"{min_size_categ}, {z_norm}, {z_outlier})"
        )
        return _execute_anomaly(conn, query, output_mode)
    finally:
        _maybe_unregister(conn, table_name, registered)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _resolve_table(
    conn: Any,
    table_or_df: Union[str, Any],
) -> tuple[str, bool]:
    """
    Return ``(table_name, was_registered)``.
    """
    if isinstance(table_or_df, str):
        return table_or_df, False
    detect_df_type(table_or_df)  # validate type before registering
    table_name = register_df_as_table(conn.native, table_or_df)
    return table_name, True


def _maybe_unregister(conn: Any, table_name: str, registered: bool) -> None:
    if registered:
        drop_df_table(conn.native, table_name)


def _execute_anomaly(conn: Any, query: str, output_mode: str) -> Union[dict, Any]:
    """Execute the query and return either a dict or a DataFrame."""
    if output_mode in ("scores", "outliers"):
        rel = conn.native.sql(query)
        return rel.df()

    # summary — convert first row to dict using column names from description
    cursor = conn.execute(query)
    col_names = [d[0] for d in cursor.description]
    rows = cursor.fetchall()

    if not rows:
        return {"status": "error", "error": "no result"}

    return dict(zip(col_names, rows[0]))
