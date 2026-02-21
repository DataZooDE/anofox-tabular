"""
PII detection, masking, and scanning wrappers.
"""

from __future__ import annotations

import json
from typing import Any, Optional, Union

from ._utils import detect_df_type, drop_df_table, register_df_as_table, relation_to_dataframe


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
        result = conn.execute(
            f"SELECT anofox_tab_pii_detect('{_escape(text_or_df)}')"
        ).fetchone()
        raw = result[0]
        return _parse_json_list(raw)

    df_type = detect_df_type(text_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    table_name = register_df_as_table(conn.native, text_or_df)
    try:
        rel = conn.native.sql(
            f"SELECT *, anofox_tab_pii_detect(\"{column}\") AS pii_detect "
            f"FROM {table_name}"
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)


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
    result = conn.execute(
        f"SELECT anofox_tab_pii_mask('{_escape(text)}', '{strategy}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


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
    result = conn.execute(
        f"SELECT anofox_tab_pii_contains('{_escape(text)}')"
    ).fetchone()
    return bool(result[0])


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
    import pandas as pd

    if columns is not None:
        cols_csv = ",".join(columns)
        query = f"SELECT * FROM anofox_tab_pii_scan_table('{table_name}', '{cols_csv}')"
    else:
        query = f"SELECT * FROM anofox_tab_pii_scan_table('{table_name}')"

    rel = conn.native.sql(query)
    return rel.df()


def pii_audit_table(
    conn: Any,
    table_name: str,
    columns: Optional[list[str]] = None,
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

    Returns
    -------
    pd.DataFrame
        Row-level PII audit with original/masked values.
    """
    if columns is not None:
        cols_csv = ",".join(columns)
        query = f"SELECT * FROM anofox_tab_pii_audit_table('{table_name}', '{cols_csv}')"
    else:
        query = f"SELECT * FROM anofox_tab_pii_audit_table('{table_name}')"

    rel = conn.native.sql(query)
    return rel.df()


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _escape(text: str) -> str:
    """Escape single quotes for inline SQL string literals."""
    return text.replace("'", "''")


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
