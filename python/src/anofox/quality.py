"""
Data quality metric wrappers.

The check functions take a *table_name*: the name of a DuckDB table or view,
or a pandas/polars DataFrame, which is copied into a temporary table for the
call (on a file-backed connection, into that database file) and dropped
afterwards::

    result = quality.volume(conn, df, min_rows=10)

Each check returns a dict with the call's parameters and every result column
of the SQL function by name: ``status``, the metric value(s), the thresholds
and ``message`` (DECIMAL values as float). ``_raw`` keeps the raw result row.
"""

from __future__ import annotations

import datetime
import functools
import inspect
import json
import numbers
from decimal import Decimal
from typing import Any, Callable, Optional, TypeVar, Union

from ._sql import quote_table_name, run
from ._table import TableLike, accepts_dataframes, without_temp_names


F = TypeVar("F", bound=Callable[..., Any])

#: The parameters of the checks that name columns.
_COLUMN_PARAMETERS = frozenset({
    "column_name", "column_names", "timestamp_column", "required_columns",
    "left_keys", "right_keys", "date_column", "count_column", "metric_column",
})


def _column_label(value: Any) -> Any:
    """An integer column label (pandas allows them) as the string DuckDB knows the column by."""
    if isinstance(value, numbers.Integral) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, (list, tuple)):
        return [_column_label(v) for v in value]
    return value


def _dataframe_check(*table_params: str) -> Callable[[F], F]:
    """
    :func:`~anofox._table.accepts_dataframes` for a check, which also takes
    integer column labels.

    A pandas DataFrame may have integer column labels (``read_csv(header=None)``);
    its copy in DuckDB names those columns ``"0"``, ``"1"``, ... . The column
    parameters are passed to SQL as those strings.
    """
    def decorator(func: F) -> F:
        signature = inspect.signature(func)

        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            bound = signature.bind(*args, **kwargs)
            for name in _COLUMN_PARAMETERS.intersection(bound.arguments):
                bound.arguments[name] = _column_label(bound.arguments[name])
            return func(*bound.args, **bound.kwargs)

        return accepts_dataframes(*table_params)(wrapper)  # type: ignore[return-value]

    return decorator



@_dataframe_check("table_name")
def volume(
    conn: Any,
    table_name: TableLike,
    min_rows: int,
    max_rows: Optional[int] = None,
) -> dict:
    """
    Check that a table has an acceptable number of rows.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    min_rows:
        Minimum acceptable row count.
    max_rows:
        Maximum acceptable row count (omit for no upper bound).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "row_count": int, "min_rows": int, "max_rows": int|None}``
    """
    if min_rows < 0:
        raise ValueError("min_rows must be >= 0")
    if max_rows is not None and max_rows < min_rows:
        raise ValueError("max_rows must be >= min_rows")

    cursor = conn.execute(
        "SELECT * FROM anofox_tab_volume(?, ?::BIGINT, ?::BIGINT)",
        [table_name, min_rows, max_rows],
    )

    return _parse_table_result(
cursor, {"min_rows": min_rows, "max_rows": max_rows})


@_dataframe_check("table_name")
def null_rate(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    max_null_rate: float = 0.0,
) -> dict:
    """
    Check that a column's null rate is within bounds.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect.
    max_null_rate:
        Maximum fraction of nulls allowed (0.0 = no nulls).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "null_rate": float, ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_null_rate(?, ?, ?::DOUBLE)", [table_name, column_name, max_null_rate]
    )
    return _parse_table_result(
cursor, {"column": column_name, "max_null_rate": max_null_rate})


@_dataframe_check("table_name")
def distinct_count(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    min_distinct: int,
    max_distinct: Optional[int] = None,
) -> dict:
    """
    Check that a column has an acceptable number of distinct values.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect.
    min_distinct:
        Minimum number of distinct values.
    max_distinct:
        Maximum number of distinct values (omit for no upper bound).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "distinct_count": int, ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_distinct_count(?, ?, ?::BIGINT, ?::BIGINT)",
        [table_name, column_name, min_distinct, max_distinct],
    )
    return _parse_table_result(
        cursor, {"column": column_name, "min_distinct": min_distinct, "max_distinct": max_distinct}
    )


@_dataframe_check("table_name")
def freshness(
    conn: Any,
    table_name: TableLike,
    timestamp_column: str,
    max_age: str,
    reference_time: Optional[str] = None,
) -> dict:
    """
    Check that the newest value of a timestamp column is recent enough.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    timestamp_column:
        Name of the timestamp column.
    max_age:
        Largest allowed age of the newest row, as an interval string, e.g. ``"24 hours"``.
    reference_time:
        Time the age is measured from, as a timestamp string or
        :class:`datetime.datetime` (default: now).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "metric_value": datetime, "threshold": datetime,
        "age_seconds": int, "message": str, ...}``. ``threshold`` is returned as a
        naive timestamp in the session time zone.
    """
    params: list = [table_name, timestamp_column, max_age]
    reference_sql = ""
    if reference_time is not None:
        reference_sql = ", ?::TIMESTAMP"
        params.append(reference_time)
    # Without reference_time the extension returns threshold as TIMESTAMPTZ, which the
    # duckdb Python client converts only with pytz installed; return it as TIMESTAMP.
    cursor = conn.execute(
        "SELECT status, metric_value, threshold::TIMESTAMP AS threshold, age_seconds, message "
        f"FROM anofox_tab_freshness(?, ?, ?::INTERVAL{reference_sql})",
        params,
    )
    return _parse_table_result(
        cursor, {"column": timestamp_column, "max_age": max_age, "reference_time": reference_time}
    )


@_dataframe_check("table_name")
def zscore(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    threshold: float = 3.0,
) -> dict:
    """
    Check numeric column for outliers using Z-score.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Numeric column to inspect.
    threshold:
        Z-score threshold (default 3.0 — standard deviations from mean).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_zscore(?, ?, ?::DOUBLE)", [table_name, column_name, threshold]
    )
    return _parse_table_result(
cursor, {"column": column_name, "threshold": threshold})


@_dataframe_check("table_name")
def iqr(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    multiplier: float = 1.5,
) -> dict:
    """
    Check numeric column for outliers using IQR method.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Numeric column to inspect.
    multiplier:
        IQR multiplier for fence calculation (default 1.5).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_iqr(?, ?, ?::DOUBLE)", [table_name, column_name, multiplier]
    )
    return _parse_table_result(
cursor, {"column": column_name, "multiplier": multiplier})


@_dataframe_check("table_name")
def schema_check(
    conn: Any,
    table_name: TableLike,
    required_columns: list[str],
) -> dict:
    """
    Check that a table contains all required columns.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    required_columns:
        List of column names that must be present.

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_schema_check(?, ?::VARCHAR[])",
        [table_name, list(required_columns)],
    )
    return _parse_table_result(
cursor, {"required_columns": required_columns})


@_dataframe_check("table_name")
def regex_match(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    pattern: str,
    min_match_rate: float = 1.0,
    max_match_rate: Optional[float] = None,
) -> dict:
    """
    Check that the share of non-NULL values matching a regex is within bounds.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect.
    pattern:
        Regular expression the values are matched against.
    min_match_rate:
        Minimum acceptable match rate (default 1.0 = all values must match).
    max_match_rate:
        Maximum acceptable match rate (omit for no upper bound).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    if max_match_rate is not None:
        cursor = conn.execute(
            "SELECT * FROM anofox_tab_regex_match(?, ?, ?, ?::DOUBLE, ?::DOUBLE)",
            [table_name, column_name, pattern, min_match_rate, max_match_rate],
        )
    else:
        cursor = conn.execute(
            "SELECT * FROM anofox_tab_regex_match(?, ?, ?, ?::DOUBLE)",
            [table_name, column_name, pattern, min_match_rate],
        )
    return _parse_table_result(
        cursor,
        {
            "column": column_name,
            "pattern": pattern,
            "min_match_rate": min_match_rate,
            "max_match_rate": max_match_rate,
        },
    )


@_dataframe_check("table_name")
def values_in_set(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    allowed_values: list[str],
    min_rate: float = 1.0,
) -> dict:
    """
    Check that the share of non-NULL values inside an allowed set is at least min_rate.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect (values are compared as strings).
    allowed_values:
        List of allowed values.
    min_rate:
        Minimum acceptable in-set rate (default 1.0 = every value must be in the set).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_values_in_set(?, ?, ?::VARCHAR[], ?::DOUBLE)",
        [table_name, column_name, [str(v) for v in allowed_values], min_rate],
    )
    return _parse_table_result(
        cursor, {"column": column_name, "allowed_values": allowed_values, "min_rate": min_rate}
    )


@_dataframe_check("table_name")
def agg_check(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    agg: str,
    lower_threshold: Optional[float] = None,
    upper_threshold: Optional[float] = None,
) -> dict:
    """
    Check that an aggregate of a numeric column is within bounds.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Numeric column to aggregate.
    agg:
        One of ``"avg"``, ``"min"``, ``"max"``, ``"sum"``, ``"median"``, ``"stddev"``.
    lower_threshold:
        Minimum acceptable aggregate value (omit for no lower bound).
    upper_threshold:
        Maximum acceptable aggregate value (omit for no upper bound).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "value": float, ...}``
    """
    if agg.lower() not in ("avg", "min", "max", "sum", "median", "stddev"):
        raise ValueError("agg must be one of 'avg', 'min', 'max', 'sum', 'median', 'stddev'")
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_agg_check(?, ?, ?, ?::DOUBLE, ?::DOUBLE)",
        [table_name, column_name, agg.lower(), lower_threshold, upper_threshold],
    )
    return _parse_table_result(
        cursor,
        {
            "column": column_name,
            "agg": agg.lower(),
            "lower_threshold": lower_threshold,
            "upper_threshold": upper_threshold,
        },
    )


@_dataframe_check("table_name")
def duplicate_count(
    conn: Any,
    table_name: TableLike,
    column_names: str | list[str],
    max_duplicates: int = 0,
) -> dict:
    """
    Check that a column (or column combination) has at most max_duplicates duplicates.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_names:
        A column name, comma-separated column names, or a list of column names.
    max_duplicates:
        Maximum acceptable number of duplicate rows (default 0).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "duplicate_count": int, ...}``
    """
    if max_duplicates < 0:
        raise ValueError("max_duplicates must be >= 0")
    cols = ",".join(column_names) if isinstance(column_names, list) else column_names
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_duplicate_count(?, ?, ?::BIGINT)",
        [table_name, cols, max_duplicates],
    )
    return _parse_table_result(
cursor, {"columns": cols, "max_duplicates": max_duplicates})


@_dataframe_check("table_name")
def occurrence(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    mode: str = "max",
    lower_threshold: Optional[int] = None,
    upper_threshold: Optional[int] = None,
) -> dict:
    """
    Check the highest or lowest frequency of any single value against thresholds.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect.
    mode:
        ``"max"`` (highest frequency, default) or ``"min"`` (lowest frequency).
    lower_threshold:
        Minimum acceptable frequency (omit for no lower bound).
    upper_threshold:
        Maximum acceptable frequency (omit for no upper bound).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "occurrence": int, ...}``
    """
    if mode.lower() not in ("max", "min"):
        raise ValueError("mode must be 'max' or 'min'")
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_occurrence(?, ?, ?, ?::BIGINT, ?::BIGINT)",
        [table_name, column_name, mode.lower(), lower_threshold, upper_threshold],
    )
    return _parse_table_result(
        cursor,
        {
            "column": column_name,
            "mode": mode.lower(),
            "lower_threshold": lower_threshold,
            "upper_threshold": upper_threshold,
        },
    )


@_dataframe_check("left_table", "right_table")
def match_rate(
    conn: Any,
    left_table: TableLike,
    right_table: TableLike,
    left_keys: str | list[str],
    right_keys: str | list[str],
    min_rate: float = 1.0,
) -> dict:
    """
    Check the share of left-table rows with a join partner in the right table.

    Doubles as a referential-integrity / foreign-key check.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    left_table:
        Name of the left DuckDB table or view, or a DataFrame (every row is checked).
    right_table:
        Name of the right DuckDB table or view, or a DataFrame (deduplicated on its key columns).
    left_keys:
        Join key column(s) on the left table (string or list).
    right_keys:
        Join key column(s) on the right table (string or list; same count as left_keys).
    min_rate:
        Minimum acceptable match rate (default 1.0 = every left row must match).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "match_rate": float, ...}``
    """
    lk = ",".join(left_keys) if isinstance(left_keys, list) else left_keys
    rk = ",".join(right_keys) if isinstance(right_keys, list) else right_keys
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_match_rate(?, ?, ?, ?, ?::DOUBLE)",
        [left_table, right_table, lk, rk, min_rate],
    )
    return _parse_table_result(
        cursor, {"left_keys": lk, "right_keys": rk, "min_rate": min_rate}
    )


@_dataframe_check("table_name")
def compliance(
    conn: Any,
    table_name: TableLike,
    expression: str,
    min_rate: float = 1.0,
) -> dict:
    """
    Check the share of rows satisfying an arbitrary SQL boolean expression.

    .. warning::
        ``expression`` is executed as SQL with the caller's privileges — pass only
        trusted expressions (same trust model as running SQL directly).

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    expression:
        SQL boolean expression evaluated per row; NULL counts as non-compliant.
    min_rate:
        Minimum acceptable compliance rate (default 1.0 = every row must comply).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "compliance_rate": float, ...}``
    """
    cursor = conn.execute(
        "SELECT * FROM anofox_tab_compliance(?, ?, ?::DOUBLE)", [table_name, expression, min_rate]
    )
    return _parse_table_result(
cursor, {"expression": expression, "min_rate": min_rate})


@_dataframe_check("table_name")
def rel_count_change(
    conn: Any,
    table_name: TableLike,
    date_column: str,
    count_column: Optional[str] = None,
    window_days: int = 7,
    lower_threshold: Optional[float] = -0.5,
    upper_threshold: Optional[float] = 0.5,
    reference_date: Optional[str] = None,
    start_date: Optional[str] = None,
    end_date: Optional[str] = None,
) -> Union[dict, list[dict]]:
    """
    Check the daily (distinct) count on a reference date against a rolling baseline.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    date_column:
        Date or timestamp column defining the daily grain.
    count_column:
        Column for ``COUNT(DISTINCT ...)`` per day; omit for row counts.
    window_days:
        Number of days strictly before the reference date used as the baseline (default 7).
    lower_threshold / upper_threshold:
        Acceptable relative change bounds (defaults -0.5/0.5; ``None`` = unbounded).
    reference_date:
        ISO date string; omit for the latest date present in the data.
    start_date / end_date:
        ISO date strings of an inclusive daily range, given together instead of
        ``reference_date``; the check is evaluated for every day of the range.

    Returns
    -------
    dict or list of dict
        Without a range: ``{"status": "pass"|"fail", "rel_change": float, ...}``.
        With ``start_date``/``end_date``: one dict per reference date (ordered by
        date), keyed by the result columns (``status``, ``reference_date``, ...).
    """
    params = [table_name, date_column, count_column, window_days,
              lower_threshold, upper_threshold, reference_date]
    sql = (
        "SELECT * FROM anofox_tab_rel_count_change("
        "?, ?, ?::VARCHAR, ?::BIGINT, ?::DOUBLE, ?::DOUBLE, ?::DATE"
        f"{_date_range_sql(start_date, end_date, params)})"
    )
    if start_date is not None or end_date is not None:
        return _fetch_dicts(conn, sql, params)
    cursor = conn.execute(sql, params)
    return _parse_table_result(
        cursor,
        {
            "date_column": date_column,
            "count_column": count_column,
            "window_days": window_days,
            "reference_date": reference_date,
        },
    )


@_dataframe_check("table_name")
def metric_anomaly_iqr(
    conn: Any,
    table_name: TableLike,
    date_column: str,
    metric_column: Optional[str] = None,
    window_days: int = 30,
    k: float = 1.5,
    mode: str = "both",
    reference_date: Optional[str] = None,
    start_date: Optional[str] = None,
    end_date: Optional[str] = None,
) -> Union[dict, list[dict]]:
    """
    Flag a reference date whose daily metric falls outside IQR bounds of the trailing window.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    date_column:
        Date or timestamp column defining the daily grain.
    metric_column:
        Column averaged per day; omit to use the daily row count.
    window_days:
        Trailing window before the reference date (default 30).
    k:
        IQR multiplier for the bounds (default 1.5).
    mode:
        ``"both"`` (default), ``"upper"`` or ``"lower"`` — which bound(s) to enforce.
    reference_date:
        ISO date string; omit for the latest date present in the data.
    start_date / end_date:
        ISO date strings of an inclusive daily range, given together instead of
        ``reference_date``; the check is evaluated for every day of the range.

    Returns
    -------
    dict or list of dict
        Without a range: ``{"status": "pass"|"fail", "metric_value": float, ...}``.
        With ``start_date``/``end_date``: one dict per reference date (ordered by
        date), keyed by the result columns (``status``, ``reference_date``, ...).
    """
    if mode.lower() not in ("both", "upper", "lower"):
        raise ValueError("mode must be 'both', 'upper' or 'lower'")
    params = [table_name, date_column, metric_column, window_days, k, mode.lower(), reference_date]
    sql = (
        "SELECT * FROM anofox_tab_metric_anomaly_iqr("
        "?, ?, ?::VARCHAR, ?::BIGINT, ?::DOUBLE, ?, ?::DATE"
        f"{_date_range_sql(start_date, end_date, params)})"
    )
    if start_date is not None or end_date is not None:
        return _fetch_dicts(conn, sql, params)
    cursor = conn.execute(sql, params)
    return _parse_table_result(
        cursor,
        {
            "date_column": date_column,
            "metric_column": metric_column,
            "window_days": window_days,
            "k": k,
            "mode": mode.lower(),
            "reference_date": reference_date,
        },
    )


@_dataframe_check("table_name")
def rolling_values_in_set(
    conn: Any,
    table_name: TableLike,
    column_name: str,
    allowed_values: list[str],
    date_column: str,
    window_days: int = 7,
    min_rate: float = 1.0,
    reference_date: Optional[str] = None,
    start_date: Optional[str] = None,
    end_date: Optional[str] = None,
) -> Union[dict, list[dict]]:
    """
    Check values-in-set membership over the trailing window ending at the reference date.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame.
    column_name:
        Column to inspect (values are compared as strings).
    allowed_values:
        List of allowed values.
    date_column:
        Date or timestamp column defining the window.
    window_days:
        Trailing window length in days, ending at the reference date inclusive (default 7).
    min_rate:
        Minimum acceptable in-set rate (default 1.0).
    reference_date:
        ISO date string; omit for the latest date present in the data.
    start_date / end_date:
        ISO date strings of an inclusive daily range, given together instead of
        ``reference_date``; the check is evaluated for every day of the range.

    Returns
    -------
    dict or list of dict
        Without a range: ``{"status": "pass"|"fail", "in_set_rate": float, ...}``.
        With ``start_date``/``end_date``: one dict per reference date (ordered by
        date), keyed by the result columns (``status``, ``reference_date``, ...).
    """
    params = [table_name, column_name, [str(v) for v in allowed_values], date_column,
              window_days, min_rate, reference_date]
    sql = (
        "SELECT * FROM anofox_tab_rolling_values_in_set("
        "?, ?, ?::VARCHAR[], ?, ?::BIGINT, ?::DOUBLE, ?::DATE"
        f"{_date_range_sql(start_date, end_date, params)})"
    )
    if start_date is not None or end_date is not None:
        return _fetch_dicts(conn, sql, params)
    cursor = conn.execute(sql, params)
    return _parse_table_result(
        cursor,
        {
            "column": column_name,
            "allowed_values": allowed_values,
            "date_column": date_column,
            "window_days": window_days,
            "min_rate": min_rate,
            "reference_date": reference_date,
        },
    )


def define_checks(conn: Any, checks: list[dict], checks_table: str = "dq_checks") -> str:
    """
    Create (or replace) a checks table for :func:`run_checks` from a list of dicts.

    Each dict supports the keys ``check_name``, ``check_type``, ``table_name`` (required)
    plus ``column_name``, ``params`` (a dict, stored as JSON; ``datetime.date`` and
    ``datetime.datetime`` values are stored as ISO 8601 strings, ``Decimal`` as a number),
    ``lower_threshold``,
    ``upper_threshold``, ``monitor_only``, ``identifier_column`` and ``filter_expr``.

    .. note::
        The checks table is read in the connection's current transaction at bind time, so
        uncommitted changes to it are used; it may be a TEMP table. DataFrame *targets* must
        be registered as real tables (see :func:`anofox._utils.register_df_as_table`).

    *checks_table* may be qualified (``"dq.checks"``, ``"db.dq.checks"``): the table is
    created in that schema, which must exist. Returns the checks table name.
    """
    checks_sql = quote_table_name(checks_table)
    run(
        conn,
        f"CREATE OR REPLACE TABLE {checks_sql} ("
        "check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR, "
        "params VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE, "
        "monitor_only BOOLEAN, identifier_column VARCHAR, filter_expr VARCHAR)"
    )
    for check in checks:
        for required in ("check_name", "check_type", "table_name"):
            if not check.get(required):
                raise ValueError(f"every check needs a non-empty '{required}'")
        params = check.get("params")
        run(
            conn,
            f"INSERT INTO {checks_sql} VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                check["check_name"],
                check["check_type"],
                check["table_name"],
                check.get("column_name"),
                json.dumps(params, default=_json_param) if params is not None else None,
                check.get("lower_threshold"),
                check.get("upper_threshold"),
                bool(check.get("monitor_only", False)),
                check.get("identifier_column"),
                check.get("filter_expr"),
            ],
        )
    return checks_table


def _json_param(value: Any) -> Any:
    """JSON form of a check parameter json.dumps cannot encode itself."""
    if isinstance(value, (datetime.date, datetime.time)):
        return value.isoformat()  # date: 2026-03-08, datetime: 2026-03-11T06:00:00
    if isinstance(value, Decimal):
        return float(value)
    raise TypeError(
        f"check parameter {value!r} of type {type(value).__qualname__} cannot be stored as JSON"
    )


def run_checks(
    conn: Any,
    checks_table: str = "dq_checks",
    result_table: Optional[str] = None,
) -> list[dict]:
    """
    Run every check defined in *checks_table* and return the uniform result rows.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    checks_table:
        Name of the checks table (see :func:`define_checks` for the schema). A table
        (TEMP too) or a regular, non-temporary view.
    result_table:
        When given, results are also appended to this table (created on first use),
        building a metric history that e.g. :func:`metric_anomaly_iqr` can analyze.
        Both names may be schema-qualified (``"dq.checks"``, ``"dq.check_results"``).

    Returns
    -------
    list[dict]
        One dict per check result with the uniform schema keys (``run_ts``, ``check_name``,
        ``check_type``, ``table_name``, ``column_name``, ``identifier``, ``value``,
        ``lower_threshold``, ``upper_threshold``, ``status``, ``message``).
    """
    if result_table is not None:
        result_sql = quote_table_name(result_table)
        run(
            conn,
            f"CREATE TABLE IF NOT EXISTS {result_sql} AS "
            "SELECT * FROM anofox_tab_run_checks(?) LIMIT 0",
            [checks_table],
        )
        run(
            conn,
            f"INSERT INTO {result_sql} SELECT * FROM anofox_tab_run_checks(?)", [checks_table]
        )
        cursor = conn.execute(
            f"SELECT * FROM {result_sql} "
            f"WHERE run_ts = (SELECT MAX(run_ts) FROM {result_sql}) ORDER BY check_name"
        )
    else:
        cursor = conn.execute(
            "SELECT * FROM anofox_tab_run_checks(?) ORDER BY check_name", [checks_table]
        )
    columns = [d[0] for d in cursor.description]
    return [dict(zip(columns, row)) for row in cursor.fetchall()]


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _date_range_sql(start_date: Optional[str], end_date: Optional[str], params: list) -> str:
    """Named start_date/end_date arguments of the time-aware checks ('' when absent).

    The dates are appended to *params*. Validation (both or neither, start <= end,
    not with reference_date) is left to the extension so the Python and SQL errors
    are the same.
    """
    sql = ""
    if start_date is not None:
        sql += ", start_date := ?::DATE"
        params.append(start_date)
    if end_date is not None:
        sql += ", end_date := ?::DATE"
        params.append(end_date)
    return sql


def _fetch_dicts(conn: Any, sql: str, params: list) -> list[dict]:
    """Run a query and return every row as a dict keyed by the result column names."""
    cursor = conn.execute(sql, params)
    columns = [d[0] for d in cursor.description]
    return [{c: _to_python(v) for c, v in zip(columns, row)} for row in cursor.fetchall()]


def _to_python(value: Any) -> Any:
    """
    Plain Python value for a result dict: DECIMAL thresholds become float, and a
    message names a DataFrame input "the DataFrame", not its temporary table.
    """
    if isinstance(value, str):
        return without_temp_names(value)
    return float(value) if isinstance(value, Decimal) else value


def _parse_table_result(cursor: Any, extra: dict) -> dict:
    """
    Convert the first row of a check's result into a result dict.

    The dict holds the call's parameters (*extra*), then every result column
    by name (``status``, the metric value(s), the thresholds, ``message``),
    with DECIMAL values converted to float. ``_raw`` keeps the row as returned
    by DuckDB, in column order, for backward compatibility.
    """
    columns = [d[0] for d in cursor.description]
    rows = cursor.fetchall()
    if not rows:
        return {"status": "error", "error": "no rows returned", **extra}

    row = rows[0]
    named = {c: _to_python(v) for c, v in zip(columns, row)}
    result: dict = {"status": str(named.get("status", "error")), **extra, **named}
    result["_raw"] = list(row)
    return result
