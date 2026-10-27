"""
PII detection, masking, and scanning wrappers.
"""

from __future__ import annotations

import json
from contextlib import contextmanager
from typing import Any, Iterator, Optional, Sequence, Union

from ._sql import fetch_frame, fetch_one, id_column_arg, quote_ident, run
from ._table import (
    DataFrame,
    TableLike,
    apply_scalar,
    check_row_input,
    is_missing,
    output_df_type,
    table_inputs,
)


def pii_detect(
    conn: Any,
    text_or_df: Union[str, Any],
    column: Optional[str] = None,
    types: Union[str, Sequence[str], None] = None,
) -> Union[Optional[list[dict]], DataFrame]:
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
    types:
        PII types to detect in this call only, as a list (``["EMAIL", "IBAN"]``)
        or a comma-separated string. ``None`` (default) uses the setting
        ``anofox_pii_enabled_types``. Leaving out the NER types (``NAME``,
        ``ORGANIZATION``, ``LOCATION``, ``MISC``) skips the NER model, which is
        much faster. See :func:`enabled_types`.

    Returns
    -------
    list[dict] or None
        When *text_or_df* is a string; ``None`` for a missing value
        (``None``, ``pandas.NA`` or NaN), like SQL ``NULL``.
    DataFrame
        When *text_or_df* is a DataFrame; adds ``pii_detect`` column.
    """
    if is_missing(text_or_df):
        return None
    if isinstance(text_or_df, str):
        with enabled_types(conn, types):
            raw = fetch_one(conn, "SELECT anofox_tab_pii_detect(?)", [text_or_df])
        return _parse_json_list(raw)

    df_type = check_row_input(text_or_df, column)

    with enabled_types(conn, types):
        return apply_scalar(
            conn, text_or_df, df_type,
            f"anofox_tab_pii_detect({quote_ident(column)})", [], "pii_detect",
            text_columns=[column],
        )


def pii_mask(
    conn: Any,
    text_or_df: Union[str, Any],
    strategy: str = "redact",
    column: Optional[str] = None,
    replace: bool = False,
    types: Union[str, Sequence[str], None] = None,
) -> Union[Optional[str], DataFrame]:
    """
    Mask all detected PII in a text or in a DataFrame column.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    text_or_df:
        Input text containing potential PII, or a pandas/polars DataFrame.
    strategy:
        Masking strategy: ``"redact"`` (default), ``"hash"``,
        ``"partial"``, or ``"asterisk"``.
    column:
        Column name when *text_or_df* is a DataFrame.
    replace:
        DataFrame only. ``False`` (default): add a ``pii_mask`` column next to
        *column*, like the other row-wise helpers, so the result still holds
        the raw value. ``True``: put the masked text in *column* itself, so
        the result no longer holds the raw value.
    types:
        PII types to detect in this call only, as a list (``["EMAIL", "IBAN"]``)
        or a comma-separated string. ``None`` (default) uses the setting
        ``anofox_pii_enabled_types``. Leaving out the NER types (``NAME``,
        ``ORGANIZATION``, ``LOCATION``, ``MISC``) skips the NER model, which is
        much faster. See :func:`enabled_types`.

    Returns
    -------
    str or None
        When *text_or_df* is a string: the text with PII replaced according to
        *strategy*; ``None`` for a missing value (``None``, ``pandas.NA`` or NaN).
    DataFrame
        When *text_or_df* is a DataFrame: a copy with an added ``pii_mask``
        column, or with *column* masked when ``replace=True`` (same index and
        row order as the input).
    """
    _check_replace(replace, text_or_df)
    if is_missing(text_or_df):
        return None
    if isinstance(text_or_df, str):
        with enabled_types(conn, types):
            value = fetch_one(conn, "SELECT anofox_tab_pii_mask(?, ?)", [text_or_df, strategy])
        return str(value) if value is not None else ""

    df_type = check_row_input(text_or_df, column)
    with enabled_types(conn, types):
        return apply_scalar(
            conn, text_or_df, df_type,
            f"anofox_tab_pii_mask({quote_ident(column)}, ?)", [strategy],
            column if replace else "pii_mask", text_columns=[column],
        )


def pii_mask_value(
    conn: Any,
    value_or_df: Union[str, None, Any],
    pii_type: str,
    strategy: str = "redact",
    column: Optional[str] = None,
    replace: bool = False,
) -> Union[Optional[str], DataFrame]:
    """
    Mask a whole value of a known PII type, without running detection.

    Use it for columns that hold PII by definition (``first_name``, ``phone``),
    where detection can miss short or unusual values such as ``"Li"``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    value_or_df:
        A value, a missing value (``None``, ``pandas.NA`` or NaN), or a
        pandas/polars DataFrame (with *column*).
    pii_type:
        The PII type the values belong to, e.g. ``"NAME"`` or ``"PHONE"``.
    strategy:
        ``"redact"`` (default), ``"hash"``, ``"partial"``, ``"asterisk"`` or ``"none"``.
    column:
        Column name when *value_or_df* is a DataFrame.
    replace:
        DataFrame only. ``False`` (default): add a ``pii_mask_value`` column.
        ``True``: put the masked values in *column* itself, so the result no
        longer holds the raw values.

    Returns
    -------
    str or None
        For a value: the masked value; ``None`` for a missing value.
    DataFrame
        For a DataFrame: a copy with an added ``pii_mask_value`` column, or
        with *column* masked when ``replace=True`` (same index and row order
        as the input).
    """
    _check_replace(replace, value_or_df)
    if is_missing(value_or_df):
        value_or_df = None
    if value_or_df is None or isinstance(value_or_df, str):
        return fetch_one(conn, "SELECT anofox_tab_pii_mask_value(?, ?, ?)", [value_or_df, pii_type, strategy])

    df_type = check_row_input(value_or_df, column)
    return apply_scalar(
        conn, value_or_df, df_type,
        f"anofox_tab_pii_mask_value(CAST({quote_ident(column)} AS VARCHAR), ?, ?)", [pii_type, strategy],
        column if replace else "pii_mask_value",
    )


def _check_replace(replace: bool, value: Any) -> None:
    """Raise ValueError when ``replace=True`` is given for a single value."""
    if replace and (is_missing(value) or isinstance(value, str)):
        raise ValueError("'replace=True' requires a DataFrame input (with column=).")


def pii_contains(
    conn: Any,
    text: Optional[str],
    types: Union[str, Sequence[str], None] = None,
) -> Optional[bool]:
    """
    Return True if *text* contains any detectable PII; ``None`` for a missing
    value (``None``, ``pandas.NA`` or NaN), like SQL ``NULL``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    text:
        Input text to inspect.
    types:
        PII types to detect in this call only, as a list (``["EMAIL", "IBAN"]``)
        or a comma-separated string. ``None`` (default) uses the setting
        ``anofox_pii_enabled_types``. Leaving out the NER types (``NAME``,
        ``ORGANIZATION``, ``LOCATION``, ``MISC``) skips the NER model, which is
        much faster. See :func:`enabled_types`.
    """
    if is_missing(text):
        return None
    with enabled_types(conn, types):
        return bool(fetch_one(conn, "SELECT anofox_tab_pii_contains(?)", [text]))


def pii_scan_table(
    conn: Any,
    table_name: TableLike,
    columns: Union[str, Sequence[str], None] = None,
    types: Union[str, Sequence[str], None] = None,
) -> DataFrame:
    """
    Scan all (or selected) string columns of a table for PII.

    Returns a DataFrame summarising PII found per column: polars for a polars
    input, else pandas.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame (copied
        into a temporary table for the call).
    columns:
        Specific columns to scan, as a list or a comma-separated string. When
        *None* all text columns are scanned (extension default behaviour).
    types:
        PII types to detect in this call only, as a list (``["EMAIL", "IBAN"]``)
        or a comma-separated string. ``None`` (default) uses the setting
        ``anofox_pii_enabled_types``. Leaving out the NER types (``NAME``,
        ``ORGANIZATION``, ``LOCATION``, ``MISC``) skips the NER model, which is
        much faster. See :func:`enabled_types`.
    """
    with enabled_types(conn, types), table_inputs(conn, table_name) as (resolved,):
        params: list = [resolved]
        if columns is not None:
            params.append(columns if isinstance(columns, str) else ",".join(columns))
        placeholders = ", ".join("?" for _ in params)
        return fetch_frame(
            conn, f"SELECT * FROM anofox_tab_pii_scan_table({placeholders})", params,
            output_df_type(table_name),
        )


def pii_audit_table(
    conn: Any,
    table_name: TableLike,
    columns: Union[str, Sequence[str], None] = None,
    id_column: Optional[str] = None,
    types: Union[str, Sequence[str], None] = None,
) -> DataFrame:
    """
    Audit a table for PII, returning row-level detail.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    table_name:
        Name of the DuckDB table or view, or a pandas/polars DataFrame (copied
        into a temporary table for the call).
    columns:
        Specific columns to audit, as a list or a comma-separated string.
    id_column:
        Optional identifier column whose value is added to every audit row,
        directly after ``row_id``.
    types:
        PII types to detect in this call only, as a list (``["EMAIL", "IBAN"]``)
        or a comma-separated string. ``None`` (default) uses the setting
        ``anofox_pii_enabled_types``. Leaving out the NER types (``NAME``,
        ``ORGANIZATION``, ``LOCATION``, ``MISC``) skips the NER model, which is
        much faster. See :func:`enabled_types`.

    Returns
    -------
    DataFrame
        Row-level PII audit with original/masked values (polars for a polars
        input, else pandas).
    """
    with enabled_types(conn, types), table_inputs(conn, table_name) as (resolved,):
        params: list = [resolved]
        if columns is not None:
            params.append(columns if isinstance(columns, str) else ",".join(columns))
        placeholders = ", ".join("?" for _ in params)
        id_arg = id_column_arg(id_column, params)
        return fetch_frame(
            conn, f"SELECT * FROM anofox_tab_pii_audit_table({placeholders}{id_arg})", params,
            output_df_type(table_name),
        )


@contextmanager
def enabled_types(conn: Any, types: Union[str, Sequence[str], None]) -> Iterator[None]:
    """
    Restrict PII detection to *types* for the ``with`` block.

    Sets ``anofox_pii_enabled_types`` to *types* (a list or a comma-separated
    string) and restores the previous value afterwards, also when the block
    raises. ``None`` leaves the setting alone. An unknown type raises the
    extension's error and leaves the setting unchanged. The setting applies to
    the whole database, so do not run detection on another thread of the same
    database meanwhile.
    """
    if types is None:
        yield
        return
    names = [t.strip() for t in (types.split(",") if isinstance(types, str) else types)]
    names = [t for t in names if t]
    if not names:
        raise ValueError("types must name at least one PII type, e.g. ['EMAIL']; use None for the setting")
    value = ",".join(names)
    previous = fetch_one(conn, "SELECT current_setting('anofox_pii_enabled_types')")
    run(conn, f"SET anofox_pii_enabled_types = {_sql_string(value)}")
    try:
        yield
    finally:
        if previous:
            run(conn, f"SET anofox_pii_enabled_types = {_sql_string(previous)}")
        else:
            run(conn, "RESET anofox_pii_enabled_types")


def _sql_string(value: str) -> str:
    """*value* as a SQL string literal (SET takes no bound parameters)."""
    return "'" + value.replace("'", "''") + "'"


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
