"""
Data quality metric wrappers.

All functions accept a *table_name* string referencing a DuckDB table or view.
To use a DataFrame, register it first::

    name = anofox._utils.register_df_as_table(conn.native, df)
    result = quality.volume(conn, name, min_rows=10)
    conn.native.unregister(name)
"""

from __future__ import annotations

import json
from typing import Any, Optional

_MAX_BIGINT = 9223372036854775807


def volume(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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

    if max_rows is not None:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_volume('{table_name}', {min_rows}, {max_rows})"
        ).fetchall()
    else:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_volume('{table_name}', {min_rows}, {_MAX_BIGINT})"
        ).fetchall()

    return _parse_table_result(rows, {"min_rows": min_rows, "max_rows": max_rows})


def null_rate(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
    column_name:
        Column to inspect.
    max_null_rate:
        Maximum fraction of nulls allowed (0.0 = no nulls).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "null_rate": float, ...}``
    """
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_null_rate('{table_name}', '{column_name}', {max_null_rate})"
    ).fetchall()
    return _parse_table_result(rows, {"column": column_name, "max_null_rate": max_null_rate})


def distinct_count(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    _max = max_distinct if max_distinct is not None else min_distinct * 1_000_000
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_distinct_count('{table_name}', '{column_name}', {min_distinct}, {_max})"
    ).fetchall()
    return _parse_table_result(
        rows, {"column": column_name, "min_distinct": min_distinct, "max_distinct": max_distinct}
    )


def freshness(
    conn: Any,
    table_name: str,
    timestamp_column: str,
    max_age: str,
    warn_age: Optional[str] = None,
) -> dict:
    """
    Check that a timestamp column is sufficiently fresh.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
    timestamp_column:
        Name of the timestamp column.
    max_age:
        Maximum acceptable age as an interval string, e.g. ``"24 hours"``.
    warn_age:
        Warning threshold age (optional).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    if warn_age is not None:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_freshness("
            f"'{table_name}', '{timestamp_column}', INTERVAL '{max_age}', INTERVAL '{warn_age}')"
        ).fetchall()
    else:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_freshness("
            f"'{table_name}', '{timestamp_column}', INTERVAL '{max_age}')"
        ).fetchall()
    return _parse_table_result(
        rows, {"column": timestamp_column, "max_age": max_age, "warn_age": warn_age}
    )


def zscore(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
    column_name:
        Numeric column to inspect.
    threshold:
        Z-score threshold (default 3.0 — standard deviations from mean).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_zscore('{table_name}', '{column_name}', {threshold})"
    ).fetchall()
    return _parse_table_result(rows, {"column": column_name, "threshold": threshold})


def iqr(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
    column_name:
        Numeric column to inspect.
    multiplier:
        IQR multiplier for fence calculation (default 1.5).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_iqr('{table_name}', '{column_name}', {multiplier})"
    ).fetchall()
    return _parse_table_result(rows, {"column": column_name, "multiplier": multiplier})


def schema_check(
    conn: Any,
    table_name: str,
    required_columns: list[str],
) -> dict:
    """
    Check that a table contains all required columns.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
    required_columns:
        List of column names that must be present.

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", ...}``
    """
    cols_list = "[" + ", ".join(f"'{c}'" for c in required_columns) + "]"
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_schema_check('{table_name}', {cols_list})"
    ).fetchall()
    return _parse_table_result(rows, {"required_columns": required_columns})


def regex_match(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    pattern_sql = pattern.replace("'", "''")
    if max_match_rate is not None:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_regex_match("
            f"'{table_name}', '{column_name}', '{pattern_sql}', {min_match_rate}, {max_match_rate})"
        ).fetchall()
    else:
        rows = conn.execute(
            f"SELECT * FROM anofox_tab_regex_match("
            f"'{table_name}', '{column_name}', '{pattern_sql}', {min_match_rate})"
        ).fetchall()
    return _parse_table_result(
        rows,
        {
            "column": column_name,
            "pattern": pattern,
            "min_match_rate": min_match_rate,
            "max_match_rate": max_match_rate,
        },
    )


def values_in_set(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    values_list = "[" + ", ".join("'" + v.replace("'", "''") + "'" for v in allowed_values) + "]"
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_values_in_set("
        f"'{table_name}', '{column_name}', {values_list}, {min_rate})"
    ).fetchall()
    return _parse_table_result(
        rows, {"column": column_name, "allowed_values": allowed_values, "min_rate": min_rate}
    )


def agg_check(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    lower_sql = "NULL" if lower_threshold is None else str(lower_threshold)
    upper_sql = "NULL" if upper_threshold is None else str(upper_threshold)
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_agg_check("
        f"'{table_name}', '{column_name}', '{agg.lower()}', {lower_sql}, {upper_sql})"
    ).fetchall()
    return _parse_table_result(
        rows,
        {
            "column": column_name,
            "agg": agg.lower(),
            "lower_threshold": lower_threshold,
            "upper_threshold": upper_threshold,
        },
    )


def duplicate_count(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_duplicate_count('{table_name}', '{cols}', {max_duplicates})"
    ).fetchall()
    return _parse_table_result(rows, {"columns": cols, "max_duplicates": max_duplicates})


def occurrence(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
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
    lower_sql = "NULL" if lower_threshold is None else str(lower_threshold)
    upper_sql = "NULL" if upper_threshold is None else str(upper_threshold)
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_occurrence("
        f"'{table_name}', '{column_name}', '{mode.lower()}', {lower_sql}, {upper_sql})"
    ).fetchall()
    return _parse_table_result(
        rows,
        {
            "column": column_name,
            "mode": mode.lower(),
            "lower_threshold": lower_threshold,
            "upper_threshold": upper_threshold,
        },
    )


def match_rate(
    conn: Any,
    left_table: str,
    right_table: str,
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
        Name of the left DuckDB table or view (every row is checked).
    right_table:
        Name of the right DuckDB table or view (deduplicated on its key columns).
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
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_match_rate("
        f"'{left_table}', '{right_table}', '{lk}', '{rk}', {min_rate})"
    ).fetchall()
    return _parse_table_result(
        rows, {"left_keys": lk, "right_keys": rk, "min_rate": min_rate}
    )


def compliance(
    conn: Any,
    table_name: str,
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
        Name of the DuckDB table or view.
    expression:
        SQL boolean expression evaluated per row; NULL counts as non-compliant.
    min_rate:
        Minimum acceptable compliance rate (default 1.0 = every row must comply).

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "compliance_rate": float, ...}``
    """
    expr_sql = expression.replace("'", "''")
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_compliance('{table_name}', '{expr_sql}', {min_rate})"
    ).fetchall()
    return _parse_table_result(rows, {"expression": expression, "min_rate": min_rate})


def rel_count_change(
    conn: Any,
    table_name: str,
    date_column: str,
    count_column: Optional[str] = None,
    window_days: int = 7,
    lower_threshold: Optional[float] = -0.5,
    upper_threshold: Optional[float] = 0.5,
    reference_date: Optional[str] = None,
) -> dict:
    """
    Check the daily (distinct) count on a reference date against a rolling baseline.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
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

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "rel_change": float, ...}``
    """
    count_sql = "NULL" if count_column is None else f"'{count_column}'"
    lower_sql = "NULL" if lower_threshold is None else str(lower_threshold)
    upper_sql = "NULL" if upper_threshold is None else str(upper_threshold)
    ref_sql = "NULL" if reference_date is None else f"DATE '{reference_date}'"
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_rel_count_change("
        f"'{table_name}', '{date_column}', {count_sql}, {window_days}, {lower_sql}, {upper_sql}, {ref_sql})"
    ).fetchall()
    return _parse_table_result(
        rows,
        {
            "date_column": date_column,
            "count_column": count_column,
            "window_days": window_days,
            "reference_date": reference_date,
        },
    )


def metric_anomaly_iqr(
    conn: Any,
    table_name: str,
    date_column: str,
    metric_column: Optional[str] = None,
    window_days: int = 30,
    k: float = 1.5,
    mode: str = "both",
    reference_date: Optional[str] = None,
) -> dict:
    """
    Flag a reference date whose daily metric falls outside IQR bounds of the trailing window.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
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

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "metric_value": float, ...}``
    """
    if mode.lower() not in ("both", "upper", "lower"):
        raise ValueError("mode must be 'both', 'upper' or 'lower'")
    metric_sql = "NULL" if metric_column is None else f"'{metric_column}'"
    ref_sql = "NULL" if reference_date is None else f"DATE '{reference_date}'"
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_metric_anomaly_iqr("
        f"'{table_name}', '{date_column}', {metric_sql}, {window_days}, {k}, '{mode.lower()}', {ref_sql})"
    ).fetchall()
    return _parse_table_result(
        rows,
        {
            "date_column": date_column,
            "metric_column": metric_column,
            "window_days": window_days,
            "k": k,
            "mode": mode.lower(),
            "reference_date": reference_date,
        },
    )


def rolling_values_in_set(
    conn: Any,
    table_name: str,
    column_name: str,
    allowed_values: list[str],
    date_column: str,
    window_days: int = 7,
    min_rate: float = 1.0,
    reference_date: Optional[str] = None,
) -> dict:
    """
    Check values-in-set membership over the trailing window ending at the reference date.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
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

    Returns
    -------
    dict
        ``{"status": "pass"|"fail", "in_set_rate": float, ...}``
    """
    values_list = "[" + ", ".join("'" + v.replace("'", "''") + "'" for v in allowed_values) + "]"
    ref_sql = "NULL" if reference_date is None else f"DATE '{reference_date}'"
    rows = conn.execute(
        f"SELECT * FROM anofox_tab_rolling_values_in_set("
        f"'{table_name}', '{column_name}', {values_list}, '{date_column}', {window_days}, {min_rate}, {ref_sql})"
    ).fetchall()
    return _parse_table_result(
        rows,
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
    plus ``column_name``, ``params`` (a dict, stored as JSON), ``lower_threshold``,
    ``upper_threshold``, ``monitor_only``, ``identifier_column`` and ``filter_expr``.

    .. note::
        The checks table must be a regular (non-temporary) table — it is read through a
        separate connection at bind time. DataFrame *targets* must be registered as real
        tables too (see :func:`anofox._utils.register_df_as_table`).

    Returns the checks table name.
    """
    conn.execute(
        f"CREATE OR REPLACE TABLE \"{checks_table}\" ("
        "check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR, "
        "params VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE, "
        "monitor_only BOOLEAN, identifier_column VARCHAR, filter_expr VARCHAR)"
    )
    for check in checks:
        for required in ("check_name", "check_type", "table_name"):
            if not check.get(required):
                raise ValueError(f"every check needs a non-empty '{required}'")
        params = check.get("params")
        conn.execute(
            f"INSERT INTO \"{checks_table}\" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                check["check_name"],
                check["check_type"],
                check["table_name"],
                check.get("column_name"),
                json.dumps(params) if params is not None else None,
                check.get("lower_threshold"),
                check.get("upper_threshold"),
                bool(check.get("monitor_only", False)),
                check.get("identifier_column"),
                check.get("filter_expr"),
            ],
        )
    return checks_table


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
        Name of the checks table (see :func:`define_checks` for the schema). Must be a
        regular (non-temporary) table.
    result_table:
        When given, results are also appended to this table (created on first use),
        building a metric history that e.g. :func:`metric_anomaly_iqr` can analyze.

    Returns
    -------
    list[dict]
        One dict per check result with the uniform schema keys (``run_ts``, ``check_name``,
        ``check_type``, ``table_name``, ``column_name``, ``identifier``, ``value``,
        ``lower_threshold``, ``upper_threshold``, ``status``, ``message``).
    """
    if result_table is not None:
        conn.execute(
            f"CREATE TABLE IF NOT EXISTS \"{result_table}\" AS "
            f"SELECT * FROM anofox_tab_run_checks('{checks_table}') LIMIT 0"
        )
        conn.execute(
            f"INSERT INTO \"{result_table}\" SELECT * FROM anofox_tab_run_checks('{checks_table}')"
        )
        cursor = conn.execute(
            f"SELECT * FROM \"{result_table}\" "
            f"WHERE run_ts = (SELECT MAX(run_ts) FROM \"{result_table}\") ORDER BY check_name"
        )
    else:
        cursor = conn.execute(
            f"SELECT * FROM anofox_tab_run_checks('{checks_table}') ORDER BY check_name"
        )
    columns = [d[0] for d in cursor.description]
    return [dict(zip(columns, row)) for row in cursor.fetchall()]


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _parse_table_result(rows: list, extra: dict) -> dict:
    """
    Convert a table-function result row list into a result dict.

    The first row contains column names from cursor description, or we rely
    on the known metric column names.  The ``status`` field comes from the
    extension itself (a VARCHAR column typically "pass" or "fail").
    """
    if not rows:
        return {"status": "error", "error": "no rows returned", **extra}

    row = rows[0]
    if len(row) == 1:
        return {"status": str(row[0]), **extra}

    # Multiple columns — build dict from the row values
    result: dict = {}
    for val in row:
        if isinstance(val, str) and val in ("pass", "fail", "warn", "error"):
            result["status"] = val
            break

    if "status" not in result:
        result["status"] = "pass" if row else "error"

    result.update(extra)
    # Attach raw values for debugging
    result["_raw"] = list(row)
    return result
