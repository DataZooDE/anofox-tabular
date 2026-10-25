"""
Email and phone number validation wrappers.

All functions accept either a scalar value (returns a scalar Python result)
or a pandas/polars DataFrame (returns a DataFrame with an added result column).
"""

from __future__ import annotations

import json
from typing import Any, Optional, Union

from ._sql import fetch_one, quote_ident
from ._table import apply_scalar
from ._utils import detect_df_type


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
        return bool(fetch_one(conn, "SELECT anofox_tab_email_is_valid(?, ?)", [email_or_df, mode]))

    # DataFrame path
    df_type = detect_df_type(email_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    return apply_scalar(
        conn, email_or_df, df_type,
        f"anofox_tab_email_is_valid({quote_ident(column)}, ?)", [mode], "email_is_valid",
    )


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
        raw = fetch_one(conn, "SELECT anofox_tab_email_validate(?, ?)", [email_or_df, mode])
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

    return apply_scalar(
        conn, email_or_df, df_type,
        f"anofox_tab_email_validate({quote_ident(column)}, ?)", [mode], "email_validate",
    )


# ---------------------------------------------------------------------------
# Phone
# ---------------------------------------------------------------------------

def phone_is_valid(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: Optional[str] = None,
    column: Optional[str] = None,
    region_column: Optional[str] = None,
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
        ISO 3166-1 alpha-2 region code used for every value, e.g. ``"DE"``.
        Defaults to ``"US"`` when neither *region* nor *region_column* is given.
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region code of each row. Cannot be
        combined with *region*.
    """
    region_sql, region_params = _region_arg(region, region_column, isinstance(phone_or_df, str))
    if isinstance(phone_or_df, str):
        return bool(fetch_one(conn, "SELECT anofox_tab_phonenumber_is_valid(?, ?)", [phone_or_df, *region_params]))

    df_type = detect_df_type(phone_or_df)
    if column is None:
        raise ValueError("'column' is required when passing a DataFrame.")

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_is_valid({quote_ident(column)}, {region_sql})", region_params,
        "phone_is_valid",
    )


def phone_parse(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: Optional[str] = None,
    column: Optional[str] = None,
    region_column: Optional[str] = None,
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
        Region hint for parsing every value, e.g. ``"DE"``. Defaults to
        ``"US"`` when neither *region* nor *region_column* is given.
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region hint of each row. Cannot be
        combined with *region*.
    """
    region_sql, region_params = _region_arg(region, region_column, isinstance(phone_or_df, str))
    if isinstance(phone_or_df, str):
        raw = fetch_one(conn, "SELECT anofox_tab_phonenumber_parse(?, ?)", [phone_or_df, *region_params])
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

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_parse({quote_ident(column)}, {region_sql})", region_params,
        "phone_parse",
    )


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
    value = fetch_one(conn, "SELECT anofox_tab_phonenumber_format(?, ?, ?)", [phone, region, fmt])
    return str(value) if value is not None else ""


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
    value = fetch_one(conn, "SELECT anofox_tab_phonenumber_region(?, ?)", [phone, hint])
    return str(value) if value is not None else ""


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _region_arg(
    region: Optional[str], region_column: Optional[str], scalar_input: bool
) -> tuple[str, list]:
    """Return the SQL and bound parameters of the region argument of a phone function."""
    if region is not None and region_column is not None:
        raise ValueError("pass either 'region' or 'region_column', not both.")
    if region_column is not None:
        if scalar_input:
            raise ValueError("'region_column' requires a DataFrame input.")
        return quote_ident(region_column), []
    return "?", [region if region is not None else "US"]
