"""
Email and phone number validation wrappers.

All functions accept either a scalar value (returns a scalar Python result)
or a pandas/polars DataFrame (returns a DataFrame with an added result column).
"""

from __future__ import annotations

import json
from typing import Any, Optional, Union

from ._utils import detect_df_type, drop_df_table, register_df_as_table, relation_to_dataframe


# ---------------------------------------------------------------------------
# Rule types for high-level AnofoxConnection.validate()
# ---------------------------------------------------------------------------

class EmailRule:
    """Validation rule for email columns."""
    def __init__(self, mode: str = "regex") -> None:
        self.mode = mode


class PhoneRule:
    """Validation rule for phone columns."""
    def __init__(self, region: str = "US") -> None:
        self.region = region


# ---------------------------------------------------------------------------
# Email
# ---------------------------------------------------------------------------

def email_is_valid(
    conn: Any,
    email_or_df: Union[str, Any],
    mode: str = "regex",
    column: Optional[str] = None,
) -> Union[bool, Any]:
    """
    Check whether an email address is valid.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    email_or_df:
        A single email string or a pandas/polars DataFrame.
    mode:
        Validation mode: ``"regex"`` (default), ``"dns"``, or ``"smtp"``.
    column:
        Column name containing emails when *email_or_df* is a DataFrame.
        Required when a DataFrame is passed.

    Returns
    -------
    bool
        When *email_or_df* is a string.
    DataFrame
        When *email_or_df* is a DataFrame; contains all original columns
        plus ``email_is_valid`` (bool).
    """
    if isinstance(email_or_df, str):
        result = conn.execute(
            f"SELECT anofox_tab_email_is_valid('{email_or_df}', '{mode}')"
        ).fetchone()
        return bool(result[0])

    # DataFrame path
    df_type = detect_df_type(email_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    table_name = register_df_as_table(conn.native, email_or_df)
    try:
        rel = conn.native.sql(
            f"SELECT *, anofox_tab_email_is_valid(\"{column}\", '{mode}') AS email_is_valid "
            f"FROM {table_name}"
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)


def email_validate(
    conn: Any,
    email_or_df: Union[str, Any],
    mode: str = "regex",
    column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Return detailed email validation results.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    email_or_df:
        A single email string or a pandas/polars DataFrame.
    mode:
        Validation mode: ``"regex"``, ``"dns"``, or ``"smtp"``.
    column:
        Column name when *email_or_df* is a DataFrame.

    Returns
    -------
    dict
        When *email_or_df* is a string.
    DataFrame
        When *email_or_df* is a DataFrame.
    """
    if isinstance(email_or_df, str):
        result = conn.execute(
            f"SELECT anofox_tab_email_validate('{email_or_df}', '{mode}')"
        ).fetchone()
        raw = result[0]
        if isinstance(raw, dict):
            return raw
        if isinstance(raw, str):
            try:
                return json.loads(raw)
            except (json.JSONDecodeError, TypeError):
                return {"raw": raw}
        return {"raw": str(raw)}

    # DataFrame path
    df_type = detect_df_type(email_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    table_name = register_df_as_table(conn.native, email_or_df)
    try:
        rel = conn.native.sql(
            f"SELECT *, anofox_tab_email_validate(\"{column}\", '{mode}') AS email_validate "
            f"FROM {table_name}"
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)


# ---------------------------------------------------------------------------
# Phone
# ---------------------------------------------------------------------------

def phone_is_valid(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: str = "US",
    column: Optional[str] = None,
) -> Union[bool, Any]:
    """
    Return True if *phone_or_df* is a valid phone number for *region*.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone_or_df:
        A single phone string or a DataFrame.
    region:
        ISO 3166-1 alpha-2 region code, e.g. ``"DE"`` or ``"US"``.
    column:
        Column name when a DataFrame is passed.
    """
    if isinstance(phone_or_df, str):
        result = conn.execute(
            f"SELECT anofox_tab_phonenumber_is_valid('{phone_or_df}', '{region}')"
        ).fetchone()
        return bool(result[0])

    df_type = detect_df_type(phone_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    table_name = register_df_as_table(conn.native, phone_or_df)
    try:
        rel = conn.native.sql(
            f"SELECT *, anofox_tab_phonenumber_is_valid(\"{column}\", '{region}') AS phone_is_valid "
            f"FROM {table_name}"
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)


def phone_parse(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: str = "US",
    column: Optional[str] = None,
) -> Union[dict, Any]:
    """
    Parse a phone number and return structured information.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone_or_df:
        A single phone string or a DataFrame.
    region:
        Default region hint for parsing, e.g. ``"DE"``.
    column:
        Column name when a DataFrame is passed.
    """
    if isinstance(phone_or_df, str):
        result = conn.execute(
            f"SELECT anofox_tab_phonenumber_parse('{phone_or_df}', '{region}')"
        ).fetchone()
        raw = result[0]
        if isinstance(raw, dict):
            return raw
        if isinstance(raw, str):
            try:
                return json.loads(raw)
            except (json.JSONDecodeError, TypeError):
                return {"raw": raw}
        return {"raw": str(raw)}

    df_type = detect_df_type(phone_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    table_name = register_df_as_table(conn.native, phone_or_df)
    try:
        rel = conn.native.sql(
            f"SELECT *, anofox_tab_phonenumber_parse(\"{column}\", '{region}') AS phone_parse "
            f"FROM {table_name}"
        )
        return relation_to_dataframe(rel, df_type)
    finally:
        drop_df_table(conn.native, table_name)


def phone_format(
    conn: Any,
    phone: str,
    region: str = "US",
    fmt: str = "E164",
) -> str:
    """
    Format a phone number string.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone:
        Phone number string.
    region:
        Default region hint, e.g. ``"DE"``.
    fmt:
        Output format: ``"E164"`` (default), ``"INTERNATIONAL"``,
        ``"NATIONAL"``, or ``"RFC3966"``.

    Returns
    -------
    str
        Formatted phone number.
    """
    result = conn.execute(
        f"SELECT anofox_tab_phonenumber_format('{phone}', '{region}', '{fmt}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def phone_region(
    conn: Any,
    phone: str,
    hint: str = "US",
) -> str:
    """
    Detect the region/country code for a phone number.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone:
        Phone number string.
    hint:
        Region hint to assist parsing, e.g. ``"DE"``.

    Returns
    -------
    str
        ISO 3166-1 alpha-2 region code.
    """
    result = conn.execute(
        f"SELECT anofox_tab_phonenumber_region('{phone}', '{hint}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""
