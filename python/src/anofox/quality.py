"""
Data quality metric wrappers.

All functions accept a *table_name* string referencing a DuckDB table or view.
To use a DataFrame, register it first::

    name = anofox._utils.register_df_as_table(conn.native, df)
    result = quality.volume(conn, name, min_rows=10)
    conn.native.unregister(name)
"""

from __future__ import annotations

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
