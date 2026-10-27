"""
Anomaly detection wrappers (Isolation Forest, DBSCAN, Outlier Tree).

Functions accept either a table name string or a DataFrame.
When ``output_mode="summary"`` they return a ``dict``; when
``output_mode="scores"`` they return a DataFrame: polars for a polars input,
pandas otherwise.
"""

from __future__ import annotations

from typing import Any, Optional, Sequence, Union

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
    seed: Optional[int] = None,
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
        Sub-sample size per tree.  When ``None`` the extension default (256) is used.
    contamination:
        Expected fraction of anomalies (default 0.1).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``.
    seed:
        Fixed random seed for reproducible scores. ``None`` (default) keeps the
        trees random, so scores change between runs.

    Returns
    -------
    dict or DataFrame
    """
    _check_output_mode("isolation_forest", output_mode)
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        params = [table_name, column, n_trees, sample_size, contamination, output_mode, seed]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest(?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?, ?::BIGINT"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, output_df_type(table_or_df))
    finally:
        cleanup_table(conn, table_name, registered)


def isolation_forest_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, Sequence[str]],
    output_mode: str = "summary",
    n_trees: int = 100,
    sample_size: Optional[int] = None,
    contamination: float = 0.1,
    id_column: Optional[str] = None,
    seed: Optional[int] = None,
    ndim: Optional[int] = None,
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
        Column names as a list, or as one comma-separated string such as
        ``"amount, items"``. A name containing a comma raises ValueError.
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
    seed:
        Fixed random seed for reproducible scores. ``None`` (default) keeps the
        trees random, so scores change between runs.
    ndim:
        Columns combined per split (default ``None`` = the extension's 1).
        Above 1 the trees split on random hyperplanes (Extended Isolation
        Forest).
    """
    _check_output_mode("isolation_forest_mv", output_mode)
    cols_csv = _columns_csv(columns)
    table_name, registered = resolve_table(conn, table_or_df)
    try:
        params = [table_name, cols_csv, n_trees, sample_size, contamination, output_mode, seed, ndim]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest_mv("
            "?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?, ?::BIGINT, ?::BIGINT"
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
        with one row per analysed row; rows with a NULL feature are left out,
        and ``row_id`` is the 1-based table row number.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    _check_output_mode("dbscan", output_mode)
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
    columns: Union[str, Sequence[str]],
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
        Numeric column names as a list, or as one comma-separated string.
        A name containing a comma raises ValueError.
    output_mode:
        ``"summary"`` → dict; ``"scores"`` (or ``"clusters"``) → DataFrame
        with one row per analysed row; rows with a NULL feature are left out,
        and ``row_id`` is the 1-based table row number.
    epsilon:
        Neighbourhood radius ε (default 0.5).
    min_points:
        Minimum cluster size (default 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    _check_output_mode("dbscan_mv", output_mode)
    cols_csv = _columns_csv(columns)
    table_name, registered = resolve_table(conn, table_or_df)
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
    columns: Union[str, Sequence[str]],
    output_mode: str = "summary",
    max_depth: Optional[int] = None,
    max_perc_outliers: Optional[float] = None,
    min_size_numeric: Optional[int] = None,
    min_size_categ: Optional[int] = None,
    z_norm: Optional[float] = None,
    z_outlier: Optional[float] = None,
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
        Column names as a list, or as one comma-separated string. A name
        containing a comma raises ValueError.
    output_mode:
        ``"summary"`` → dict with counts; ``"outliers"`` → DataFrame with
        per-row explanations.
    max_depth:
        Maximum tree depth.
    max_perc_outliers:
        Largest share of a group that may be flagged.
    min_size_numeric:
        Smallest group for a numeric target.
    min_size_categ:
        Smallest group for a categorical target.
    z_norm:
        Z threshold for the group's confidence interval.
    z_outlier:
        Z threshold to flag a value.

        Each parameter left at ``None`` uses the extension's default, the same
        as in SQL: ``max_depth=4``, ``max_perc_outliers=0.01``,
        ``min_size_numeric=25``, ``min_size_categ=75``, ``z_norm=2.67``,
        ``z_outlier=8.0``. Lower the size and z thresholds for small tables.
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="outliers"``.

    Returns
    -------
    dict or DataFrame
    """
    _check_output_mode("outlier_tree", output_mode)
    cols_csv = _columns_csv(columns)
    table_name, registered = resolve_table(conn, table_or_df)
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


def _columns_csv(columns: Union[str, Sequence[str]]) -> str:
    """
    The comma-separated column list the extension expects, from a string or a sequence.

    The extension splits the list at every comma (there is no quoting), so a
    column whose name contains a comma cannot be selected: it raises ValueError.
    """
    if isinstance(columns, str):
        return columns
    for column in columns:
        if "," in column:
            raise ValueError(
                f"column {column!r} contains a comma; the multivariate anomaly functions "
                "take a comma-separated column list and cannot select it. Rename the column."
            )
    return ",".join(columns)


_OUTPUT_MODES = {
    "isolation_forest": ("summary", "scores"),
    "isolation_forest_mv": ("summary", "scores"),
    # 'scores' is the package-wide name of the per-row mode; the SQL name is 'clusters'.
    "dbscan": ("summary", "clusters", "scores"),
    "dbscan_mv": ("summary", "clusters", "scores"),
    "outlier_tree": ("summary", "outliers"),
}


def _check_output_mode(function: str, output_mode: str) -> None:
    """Raise ValueError unless *output_mode* is a mode of *function*."""
    allowed = _OUTPUT_MODES[function]
    if output_mode not in allowed:
        raise ValueError(
            f"{function}: output_mode must be one of {', '.join(map(repr, allowed))}, "
            f"got {output_mode!r}"
        )


def _execute_anomaly(
    conn: Any, query: str, params: list, output_mode: str, df_type: str
) -> Union[dict, Any]:
    """Execute the query: a dict for 'summary', else a DataFrame of *df_type* (one row per row)."""
    if output_mode != "summary":
        return fetch_frame(conn, query, params, df_type)

    # summary — convert first row to dict using column names from description
    cursor = conn.execute(query, params)
    col_names = [d[0] for d in cursor.description]
    rows = cursor.fetchall()

    if not rows:
        return {"status": "error", "error": "no result"}

    return dict(zip(col_names, rows[0]))
