"""
PII detection, masking, and scanning wrappers.
"""

from __future__ import annotations

import json
from typing import Any, Optional, Union

from ._sql import fetch_frame, fetch_one, id_column_arg
from ._table import apply_scalar
from ._utils import detect_df_type


def pii_detect(
    conn: Any,
    text_or_df: Union[str, Any],
    column: Optional[str] = None,
) -> Union[list[dict], Any]:
    """
    Detect PII entities in text.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    text_or_df:
        A plain text string or a pandas/polars DataFrame.
    column:
        Column name when *text_or_df* is a DataFrame.

    Returns
    -------
    list[dict]
        When *text_or_df* is a string.
    DataFrame
        When *text_or_df* is a DataFrame; adds ``pii_detect`` column.
    """
    if isinstance(text_or_df, str):
        raw = fetch_one(conn, "SELECT anofox_tab_pii_detect(?)", [text_or_df])
        return _parse_json_list(raw)

    df_type = detect_df_type(text_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    return apply_scalar(conn, text_or_df, df_type, "anofox_tab_pii_detect", column, [], "pii_detect")


def pii_mask(
    conn: Any,
    text: str,
    strategy: str = "redact",
) -> str:
    """
    Mask all detected PII in *text*.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    text:
        Input text containing potential PII.
    strategy:
        Masking strategy: ``"redact"`` (default), ``"hash"``,
        ``"partial"``, or ``"asterisk"``.

    Returns
    -------
    str
        Text with PII replaced according to *strategy*.
    """
    value = fetch_one(conn, "SELECT anofox_tab_pii_mask(?, ?)", [text, strategy])
    return str(value) if value is not None else ""


def pii_contains(
    conn: Any,
    text: str,
) -> bool:
    """
    Return True if *text* contains any detectable PII.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    text:
        Input text to inspect.
    """
    return bool(fetch_one(conn, "SELECT anofox_tab_pii_contains(?)", [text]))


def pii_scan_table(
    conn: Any,
    table_name: str,
    columns: Optional[list[str]] = None,
) -> Any:
    """
    Scan all (or selected) string columns of a table for PII.

    Returns a pandas DataFrame summarising PII found per column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
    columns:
        Specific columns to scan.  When *None* all text columns are
        scanned (extension default behaviour).
    """
    params: list = [table_name]
    if columns is not None:
        params.append(",".join(columns))
    placeholders = ", ".join("?" for _ in params)
    return fetch_frame(conn, f"SELECT * FROM anofox_tab_pii_scan_table({placeholders})", params)


def pii_audit_table(
    conn: Any,
    table_name: str,
    columns: Optional[list[str]] = None,
    id_column: Optional[str] = None,
) -> Any:
    """
    Audit a table for PII, returning row-level detail.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view.
    columns:
        Specific columns to audit.
    id_column:
        Optional identifier column whose value is added to every audit row,
        directly after ``row_id``.

    Returns
    -------
    pd.DataFrame
        Row-level PII audit with original/masked values.
    """
    params: list = [table_name]
    if columns is not None:
        params.append(",".join(columns))
    placeholders = ", ".join("?" for _ in params)
    id_arg = id_column_arg(id_column, params)
    return fetch_frame(
        conn, f"SELECT * FROM anofox_tab_pii_audit_table({placeholders}{id_arg})", params
    )


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _parse_json_list(raw: Any) -> list[dict]:
    """Convert raw DuckDB return value to a list of dicts."""
    if raw is None:
        return []
    if isinstance(raw, list):
        return raw
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, list):
                return parsed
            return [parsed]
        except (json.JSONDecodeError, TypeError):
            return [{"raw": raw}]
    return [{"raw": str(raw)}]
