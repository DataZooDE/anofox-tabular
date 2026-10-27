"""
Anomaly detection wrappers (Isolation Forest, DBSCAN, Outlier Tree).

Functions accept either a table name string or a DataFrame.
When ``output_mode="summary"`` they return a ``dict``; when
``output_mode="scores"`` they return a DataFrame: polars for a polars input,
pandas otherwise.

A row with a ``NULL`` in a selected column is left out. A float NaN in a
DataFrame is left out too, for pandas and polars alike (pandas copies it as
``NULL``; the wrappers copy a polars NaN as ``NULL`` as well).
"""

from __future__ import annotations

import warnings
from typing import Any, Optional, Sequence, Union

from ._sql import fetch_frame, id_column_arg, quote_ident
from ._table import DataFrame, output_df_type, table_inputs


#: Fewest scored rows for which ``isolation_forest`` and ``isolation_forest_mv``
#: do not warn. With fewer rows the trees cannot tell an unusual value from a
#: typical one: the scores are close to each other (two rows always score the
#: same) and usually no row is flagged.
MIN_ISOLATION_FOREST_ROWS = 10


def isolation_forest(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    n_trees: Optional[int] = None,
    sample_size: Optional[int] = None,
    contamination: Optional[float] = None,
    id_column: Optional[str] = None,
    seed: Optional[int] = None,
) -> Union[dict, DataFrame]:
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
        Number of isolation trees (``None``: the extension default, 100).
    sample_size:
        Sub-sample size per tree.  When ``None`` the extension default (256) is used.
    contamination:
        Expected fraction of anomalies (``None``: the extension default, 0.1).
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
        The summary's ``avg_value`` is ``None`` when no row was scored
        (``total_count`` 0). A DataFrame column of only ``None`` raises
        ValueError. The per-row output names the scored column ``value``,
        as SQL does, whatever the input column is called.

    Warns
    -----
    UserWarning
        When 1 to ``MIN_ISOLATION_FOREST_ROWS - 1`` (9) rows were scored: so
        few rows give scores that say little, and usually nothing is flagged.
    """
    _check_output_mode("isolation_forest", output_mode)
    _check_id_column("isolation_forest", output_mode, id_column)
    table_or_df = _nan_as_null(table_or_df)
    with table_inputs(conn, table_or_df) as (table_name,):
        _check_has_values(conn, table_or_df, table_name, [column])
        params = [table_name, column, n_trees, sample_size, contamination, output_mode, seed]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest(?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?, ?::BIGINT"
            f"{id_column_arg(id_column, params)})"
        )
        result = _execute_anomaly(conn, query, params, output_mode, table_or_df)
    _warn_few_rows("isolation_forest", result)
    return result


def isolation_forest_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, Sequence[str]],
    output_mode: str = "summary",
    n_trees: Optional[int] = None,
    sample_size: Optional[int] = None,
    contamination: Optional[float] = None,
    id_column: Optional[str] = None,
    seed: Optional[int] = None,
    ndim: Optional[int] = None,
) -> Union[dict, DataFrame]:
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
        Number of isolation trees (``None``: the extension default, 100).
    sample_size:
        Sub-sample size per tree (``None``: the extension default, 256).
    contamination:
        Expected fraction of anomalies (``None``: the extension default, 0.1).
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

    Warns
    -----
    UserWarning
        When fewer than ``MIN_ISOLATION_FOREST_ROWS`` (10) rows were scored, as
        :func:`isolation_forest`.
    """
    _check_output_mode("isolation_forest_mv", output_mode)
    _check_id_column("isolation_forest_mv", output_mode, id_column)
    cols_csv = _columns_csv(columns)
    table_or_df = _nan_as_null(table_or_df)
    with table_inputs(conn, table_or_df) as (table_name,):
        _check_has_values(conn, table_or_df, table_name, cols_csv.split(","))
        params = [table_name, cols_csv, n_trees, sample_size, contamination, output_mode, seed, ndim]
        query = (
            "SELECT * FROM anofox_tab_isolation_forest_mv("
            "?, ?, ?::BIGINT, ?::BIGINT, ?::DOUBLE, ?, ?::BIGINT, ?::BIGINT"
            f"{id_column_arg(id_column, params)})"
        )
        result = _execute_anomaly(conn, query, params, output_mode, table_or_df)
    _warn_few_rows("isolation_forest_mv", result)
    return result


def dbscan(
    conn: Any,
    table_or_df: Union[str, Any],
    column: str,
    output_mode: str = "summary",
    epsilon: Optional[float] = None,
    min_points: Optional[int] = None,
    id_column: Optional[str] = None,
) -> Union[dict, DataFrame]:
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
        and ``row_id`` is the 1-based table row number. The clustered column
        is named ``value`` in it, as in SQL, whatever the input column is called.
    epsilon:
        Neighbourhood radius ε (``None``: the extension default, 0.5).
    min_points:
        Minimum cluster size (``None``: the extension default, 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    _check_output_mode("dbscan", output_mode)
    _check_id_column("dbscan", output_mode, id_column)
    # The extension's per-row DBSCAN mode is named 'clusters'; keep the
    # package-level "scores" naming consistent with the other detectors.
    sql_mode = "clusters" if output_mode == "scores" else output_mode
    table_or_df = _nan_as_null(table_or_df)
    with table_inputs(conn, table_or_df) as (table_name,):
        _check_has_values(conn, table_or_df, table_name, [column])
        params = [table_name, column, epsilon, min_points, sql_mode]
        query = (
            "SELECT * FROM anofox_tab_dbscan(?, ?, ?::DOUBLE, ?::BIGINT, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, table_or_df)


def dbscan_mv(
    conn: Any,
    table_or_df: Union[str, Any],
    columns: Union[str, Sequence[str]],
    output_mode: str = "summary",
    epsilon: Optional[float] = None,
    min_points: Optional[int] = None,
    id_column: Optional[str] = None,
) -> Union[dict, DataFrame]:
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
        Neighbourhood radius ε (``None``: the extension default, 0.5).
    min_points:
        Minimum cluster size (``None``: the extension default, 5).
    id_column:
        Optional identifier column of the input whose value is added to the
        per-row output, directly after ``row_id``. Only valid with
        ``output_mode="scores"``/``"clusters"``.
    """
    _check_output_mode("dbscan_mv", output_mode)
    _check_id_column("dbscan_mv", output_mode, id_column)
    cols_csv = _columns_csv(columns)
    sql_mode = "clusters" if output_mode == "scores" else output_mode
    table_or_df = _nan_as_null(table_or_df)
    with table_inputs(conn, table_or_df) as (table_name,):
        _check_has_values(conn, table_or_df, table_name, cols_csv.split(","))
        params = [table_name, cols_csv, epsilon, min_points, sql_mode]
        query = (
            "SELECT * FROM anofox_tab_dbscan_mv(?, ?, ?::DOUBLE, ?::BIGINT, ?"
            f"{id_column_arg(id_column, params)})"
        )
        return _execute_anomaly(conn, query, params, output_mode, table_or_df)


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
) -> Union[dict, DataFrame]:
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
        per-row explanations. Its ``conditions`` column holds, per outlier,
        the list of conditions that define the group, each a dict with the
        keys ``column``, ``type`` (``"numeric"`` or ``"categorical"``),
        ``operator`` (``"<="``, ``">"``, ``"in"``), ``value`` (the numeric
        threshold, else ``None``) and ``values`` (the categories, else
        ``None``); in polars a ``List(Struct)``. SQL returns the same
        conditions as JSON text. Without outliers the DataFrame has no rows
        and the same dtypes (``conditions`` stays an ``object`` column).
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
    _check_id_column("outlier_tree", output_mode, id_column)
    cols_csv = _columns_csv(columns)
    table_or_df = _nan_as_null(table_or_df)
    with table_inputs(conn, table_or_df) as (table_name,):
        params = [table_name, cols_csv, output_mode, max_depth, max_perc_outliers,
                  min_size_numeric, min_size_categ, z_norm, z_outlier]
        source = (
            "anofox_tab_outlier_tree(?, ?, ?, ?::INTEGER, ?::DOUBLE, "
            "?::INTEGER, ?::INTEGER, ?::DOUBLE, ?::DOUBLE"
            f"{id_column_arg(id_column, params)})"
        )
        if output_mode == "summary":
            query = f"SELECT * FROM {source}"
        else:
            # The extension returns the conditions as JSON text; parse them into
            # one struct (dict) per condition with a fixed set of keys.
            query = (
                f"SELECT * REPLACE (from_json(conditions, '{_CONDITIONS_SCHEMA}') AS conditions) "
                f"FROM {source}"
            )
        result = _execute_anomaly(conn, query, params, output_mode, table_or_df)
        if output_mode != "summary" and output_df_type(table_or_df) == "pandas":
            # DuckDB hands a LIST to pandas as a numpy array; give plain lists.
            # An explicit object dtype: without rows pandas would make it float64.
            import pandas as pd
            result["conditions"] = pd.Series(
                [list(c) if c is not None else None for c in result["conditions"]],
                index=result.index, dtype=object,
            )
        return result


#: The keys of one outlier_tree condition: ``value`` is the threshold of a numeric
#: split (``None`` for a categorical one), ``values`` the categories of a
#: categorical split (``None`` for a numeric one).
_CONDITIONS_SCHEMA = (
    '[{"column":"VARCHAR","type":"VARCHAR","operator":"VARCHAR",'
    '"value":"DOUBLE","values":["VARCHAR"]}]'
)


def _nan_as_null(table_or_df: Any) -> Any:
    """
    A polars DataFrame with each float NaN replaced by null; anything else unchanged.

    pandas turns NaN into SQL ``NULL`` when the DataFrame is copied into
    DuckDB, polars keeps it as a NaN value, which the detectors treat
    differently from a NULL (DBSCAN calls it noise, OutlierTree counts it). As
    a null, a NaN row is left out by every detector, for both libraries. The
    caller's DataFrame is not changed.
    """
    if isinstance(table_or_df, str) or type(table_or_df).__module__.split(".")[0] != "polars":
        return table_or_df
    import polars as pl
    if not hasattr(table_or_df, "with_columns"):  # a polars object that is not a DataFrame
        return table_or_df
    return table_or_df.with_columns(pl.col(pl.Float32, pl.Float64).fill_nan(None))


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


def _check_has_values(conn: Any, table_or_df: Any, table_name: str, columns: Sequence[str]) -> None:
    """
    Raise ValueError for a DataFrame column that holds no value at all.

    A pandas/polars column of only ``None`` has no type: DuckDB copies it as
    type ``NULL`` (2.0) or as an arbitrary type (1.5), and the extension then
    reports a type error that hides the actual problem. The DataFrame itself is
    inspected, so the check is the same on every DuckDB version. A typed float
    column of only NaN is not affected: it is analysed like SQL does (an empty
    summary). Table names are passed through unchanged.
    """
    if isinstance(table_or_df, str):
        return
    for name in (c.strip() for c in columns):
        if name not in table_or_df.columns:
            continue
        series = table_or_df[name]
        if _is_untyped_all_missing(series):
            raise ValueError(
                f"column {name!r} has no non-null values; the anomaly detectors need "
                "at least one number in it"
            )


def _is_untyped_all_missing(series: Any) -> bool:
    """True for a pandas/polars column without a numeric type that holds only missing values."""
    if len(series) == 0:
        return False
    module = type(series).__module__
    if module.startswith("polars"):
        import polars as pl
        return series.dtype == pl.Null or (not series.dtype.is_numeric() and series.null_count() == len(series))
    # pandas: an object column of only None/NaN (a float column of only NaN keeps its type)
    return series.dtype == object and bool(series.isna().all())


def _warn_few_rows(function: str, result: Any) -> None:
    """Warn when an Isolation Forest scored 1 to MIN_ISOLATION_FOREST_ROWS - 1 rows."""
    scored = result.get("total_count") if isinstance(result, dict) else len(result)
    if scored is None or not 0 < scored < MIN_ISOLATION_FOREST_ROWS:
        return
    warnings.warn(
        f"{function}: only {scored} rows were scored; an Isolation Forest needs at least "
        f"{MIN_ISOLATION_FOREST_ROWS} to tell an unusual value from a typical one, so the "
        "scores are not meaningful (rows tend to score alike and none is flagged)",
        UserWarning,
        stacklevel=3,
    )


def _stable_empty_dtypes(result: Any) -> Any:
    """
    A pandas *result* without rows with the dtypes it would have with rows.

    DuckDB gives a ``VARCHAR`` column without rows the dtype ``object``, while
    pandas infers its string dtype for one with rows (``str`` on pandas 3). The
    ``object`` columns of an empty result are text columns (outlier_tree's
    ``conditions`` list column is fixed by its caller).
    """
    if len(result) > 0:
        return result
    import pandas as pd
    text_dtype = pd.Series(["text"]).dtype
    for column in result.columns:
        if result[column].dtype == object:
            result[column] = result[column].astype(text_dtype)
    return result


def _with_input_index(result: Any, table_or_df: Any) -> Any:
    """
    Give the per-row *result* of a pandas DataFrame input that input's index labels.

    ``row_id`` is the 1-based row number of the input, and rows with a NULL
    feature are left out of the result, so a fresh 0..k-1 index would assign
    scores to the wrong rows on ``df[col] = result[col]``. With the input's
    labels the assignment aligns, and the rows left out become missing. A
    polars result keeps ``row_id`` (polars has no index); a table name input
    keeps the default index.
    """
    if isinstance(table_or_df, str) or output_df_type(table_or_df) != "pandas":
        return result
    positions = result["row_id"].to_numpy(dtype="int64") - 1
    result.index = table_or_df.index.take(positions)
    return result


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


def _check_id_column(function: str, output_mode: str, id_column: Optional[str]) -> None:
    """
    Raise ValueError for an *id_column* with the summary output.

    The extension rejects it as well, but with a BinderException that quotes
    the wrapper's internal query.
    """
    if id_column is not None and output_mode == "summary":
        per_row = "outliers" if function == "outlier_tree" else "scores"
        raise ValueError(
            f"{function}: id_column needs output_mode='{per_row}'; the summary has "
            "no per-row output to add it to"
        )


def _execute_anomaly(
    conn: Any, query: str, params: list, output_mode: str, table_or_df: Any
) -> Union[dict, DataFrame]:
    """
    Execute the query: a dict for 'summary', else a DataFrame with one row per
    analysed row (polars for a polars input, else pandas).
    """
    if output_mode != "summary":
        df_type = output_df_type(table_or_df)
        result = fetch_frame(conn, query, params, df_type)
        if df_type == "pandas":
            result = _stable_empty_dtypes(result)
        return _with_input_index(result, table_or_df)

    # summary — convert first row to dict using column names from description
    cursor = conn.execute(query, params)
    col_names = [d[0] for d in cursor.description]
    rows = cursor.fetchall()

    if not rows:
        return {"status": "error", "error": "no result"}

    summary = dict(zip(col_names, rows[0]))
    # The mean of no values is undefined: report None, as documented, also
    # where the extension returns 0.0 for an empty input.
    if "avg_value" in summary and summary.get("total_count") == 0:
        summary["avg_value"] = None
    return summary
