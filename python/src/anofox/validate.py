"""
Email and phone number validation wrappers.

All functions accept either a scalar value (returns a scalar Python result)
or a pandas/polars DataFrame (returns a DataFrame with an added result column).
"""

from __future__ import annotations

import json
import numbers
import warnings
from typing import Any, Optional, Union

from ._sql import fetch_one, quote_ident
from ._table import DataFrame, apply_scalar, check_row_input, frame_column, is_missing


# ---------------------------------------------------------------------------
# Rule types for high-level AnofoxConnection.validate()
# ---------------------------------------------------------------------------

class EmailRule:
    """Validation rule for email columns."""
    def __init__(self, mode: str = "regex") -> None:
        self.mode = mode


class PhoneRule:
    """Validation rule for phone columns.

    ``region`` is the region hint for numbers without ``+``; ``None`` (the
    default) uses the ``anofox_tab_phonenumber_default_region`` setting, like
    the phone helpers. ``region_column`` instead names a column of the
    DataFrame that holds each row's region code, like
    :func:`phone_is_valid` with ``region_column=``. Pass at most one of the two.
    """
    def __init__(self, region: Optional[str] = None, region_column: Optional[str] = None) -> None:
        if region is not None and region_column is not None:
            raise ValueError("PhoneRule: pass either 'region' or 'region_column', not both.")
        self.region = region
        self.region_column = region_column


# ---------------------------------------------------------------------------
# Email
# ---------------------------------------------------------------------------

def email_is_valid(
    conn: Any,
    email_or_df: Union[str, Any],
    mode: str = "regex",
    column: Optional[str] = None,
) -> Union[Optional[bool], DataFrame]:
    """
    Check whether an email address is valid.

    With mode ``"dns"`` or ``"smtp"`` a check that cannot finish (a timeout,
    an unreachable server) gives ``None`` rather than ``False``, like SQL
    ``email_is_valid``; ``email_validate`` returns the reason.

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
    bool or None
        When *email_or_df* is a string; ``None`` for an unknown outcome.
    DataFrame
        When *email_or_df* is a DataFrame; contains all original columns
        plus ``email_is_valid`` (bool, missing for an unknown outcome).
    """
    if is_missing(email_or_df):
        return None
    if isinstance(email_or_df, str):
        result = fetch_one(conn, "SELECT anofox_tab_email_is_valid(?, ?)", [email_or_df, mode])
        return None if result is None else bool(result)

    # DataFrame path
    df_type = check_row_input(email_or_df, column)

    return apply_scalar(
        conn, email_or_df, df_type,
        f"anofox_tab_email_is_valid({quote_ident(column)}, ?)", [mode], "email_is_valid",
        text_columns=[column],
    )


def email_validate(
    conn: Any,
    email_or_df: Union[str, Any],
    mode: str = "regex",
    column: Optional[str] = None,
) -> Union[Optional[dict], DataFrame]:
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
    dict or None
        When *email_or_df* is a string: ``valid``, ``stage``, ``reason``,
        ``mx_hosts`` and ``smtp_debug``; ``None`` for a missing value.
    DataFrame
        When *email_or_df* is a DataFrame: a copy with an added
        ``email_validate`` column holding one such dict per row (a missing
        value for a missing email); in polars a ``Struct`` column. To get the
        fields as columns::

            checked = out["email_validate"].dropna()           # pandas
            details = pd.DataFrame(checked.tolist(), index=checked.index)
            out = out.join(details[["valid", "stage", "reason"]])

            out = out.unnest("email_validate")                 # polars
    """
    if is_missing(email_or_df):
        return None
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
    df_type = check_row_input(email_or_df, column)

    return apply_scalar(
        conn, email_or_df, df_type,
        f"anofox_tab_email_validate({quote_ident(column)}, ?)", [mode], "email_validate",
        text_columns=[column],
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
) -> Union[Optional[bool], DataFrame]:
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
        Default ``None``: the setting ``anofox_tab_phonenumber_default_region``
        applies (``US`` unless changed).
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region code of each row. Cannot be
        combined with *region*.
    """
    _check_phone_scalar(phone_or_df)
    region_sql, region_params = _region_arg(region, region_column, _is_scalar(phone_or_df))
    if is_missing(phone_or_df):
        return None
    if isinstance(phone_or_df, str):
        return bool(fetch_one(conn, "SELECT anofox_tab_phonenumber_is_valid(?, ?)", [phone_or_df, *region_params]))

    df_type = _phone_row_input(phone_or_df, column)

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_is_valid({quote_ident(column)}, {region_sql})", region_params,
        "phone_is_valid", text_columns=[column, region_column],
    )


def phone_parse(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: Optional[str] = None,
    column: Optional[str] = None,
    region_column: Optional[str] = None,
) -> Union[Optional[dict], DataFrame]:
    """
    Parse a phone number and return structured information.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone_or_df:
        A single phone string or a DataFrame.
    region:
        Region hint for parsing every value, e.g. ``"DE"``. Default ``None``:
        the setting ``anofox_tab_phonenumber_default_region`` applies.
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region hint of each row. Cannot be
        combined with *region*.
    """
    _check_phone_scalar(phone_or_df)
    region_sql, region_params = _region_arg(region, region_column, _is_scalar(phone_or_df))
    if is_missing(phone_or_df):
        return None
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

    df_type = _phone_row_input(phone_or_df, column)

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_parse({quote_ident(column)}, {region_sql})", region_params,
        "phone_parse", text_columns=[column, region_column],
    )


def phone_format(
    conn: Any,
    phone_or_df: Union[str, Any],
    region: Optional[str] = None,
    format: Optional[str] = None,  # noqa: A002 - the SQL argument's name
    column: Optional[str] = None,
    region_column: Optional[str] = None,
    *,
    fmt: Optional[str] = None,
) -> Union[Optional[str], DataFrame]:
    """
    Format a phone number, like SQL ``phonenumber_format(phone, region, format)``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone_or_df:
        A single phone string or a pandas/polars DataFrame.
    region:
        Region hint for every value, e.g. ``"DE"``. Default ``None``: the
        setting ``anofox_tab_phonenumber_default_region`` applies (``US``
        unless changed).
    format:
        Output format: ``"E164"`` (default), ``"INTERNATIONAL"``,
        ``"NATIONAL"``, or ``"RFC3966"``; an unknown format raises a DuckDB error.
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region hint of each row. Cannot be
        combined with *region*.
    fmt:
        Deprecated alias of *format* (emits a :class:`DeprecationWarning`);
        passing both raises :class:`TypeError`.

    Returns
    -------
    str or None
        When *phone_or_df* is a string; ``None`` unless the number is valid
        (the ``phone_is_valid`` check); ``phone_parse`` gives the reason.
    DataFrame
        When *phone_or_df* is a DataFrame: a copy with an added
        ``phone_format`` column (input index and row order kept).
    """
    if fmt is not None:
        if format is not None:
            raise TypeError("phone_format() got both 'format' and its deprecated alias 'fmt'; pass only format=")
        warnings.warn(
            "phone_format(fmt=...) is deprecated; use format=", DeprecationWarning, stacklevel=2
        )
        format = fmt
    if format is None:
        format = "E164"
    _check_phone_scalar(phone_or_df)
    region_sql, region_params = _region_arg(region, region_column, _is_scalar(phone_or_df))
    if is_missing(phone_or_df):
        return None
    if isinstance(phone_or_df, str):
        return fetch_one(
            conn, "SELECT anofox_tab_phonenumber_format(?, ?, ?)", [phone_or_df, *region_params, format]
        )

    df_type = _phone_row_input(phone_or_df, column)

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_format({quote_ident(column)}, {region_sql}, ?)",
        [*region_params, format], "phone_format", text_columns=[column, region_column],
    )


def phone_region(
    conn: Any,
    phone_or_df: Union[str, Any],
    hint: Optional[str] = None,
    column: Optional[str] = None,
    region_column: Optional[str] = None,
) -> Union[Optional[str], DataFrame]:
    """
    Detect the region of a phone number, like SQL ``phonenumber_region(phone, region)``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    phone_or_df:
        A single phone string or a pandas/polars DataFrame.
    hint:
        Region hint for every value, e.g. ``"DE"``. Default ``None``: the
        setting ``anofox_tab_phonenumber_default_region`` applies.
    column:
        Column name when a DataFrame is passed.
    region_column:
        DataFrame only: column holding the region hint of each row. Cannot be
        combined with *hint*.

    Returns
    -------
    str or None
        ISO 3166-1 alpha-2 region code when *phone_or_df* is a string;
        ``None`` unless the number is valid.
    DataFrame
        When *phone_or_df* is a DataFrame: a copy with an added
        ``phone_region`` column.
    """
    _check_phone_scalar(phone_or_df)
    region_sql, region_params = _region_arg(hint, region_column, _is_scalar(phone_or_df))
    if is_missing(phone_or_df):
        return None
    if isinstance(phone_or_df, str):
        return fetch_one(conn, "SELECT anofox_tab_phonenumber_region(?, ?)", [phone_or_df, *region_params])

    df_type = _phone_row_input(phone_or_df, column)

    return apply_scalar(
        conn, phone_or_df, df_type,
        f"anofox_tab_phonenumber_region({quote_ident(column)}, {region_sql})", region_params,
        "phone_region", text_columns=[column, region_column],
    )


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _is_scalar(value: Any) -> bool:
    """True for a single value (a string or a missing value), False for a DataFrame."""
    return isinstance(value, str) or is_missing(value)


_PHONE_NOT_TEXT = (
    "phone numbers must be text (a leading 0 or + is part of the number); "
    "convert with astype(str)"
)


def _check_phone_scalar(value: Any) -> None:
    """Raise TypeError for a single phone number given as a number instead of text."""
    if isinstance(value, numbers.Number) and not isinstance(value, bool) and not is_missing(value):
        raise TypeError(f"{_PHONE_NOT_TEXT}, got {type(value).__qualname__} {value!r}")


def _phone_row_input(df: Any, column: Optional[str]) -> str:
    """
    :func:`~anofox._table.check_row_input` for a phone column, which must hold text.

    A numeric column is rejected rather than converted: converting would lose
    leading zeros (``030123456`` read as the integer ``30123456``). A column
    holding only missing values is accepted (it is passed as text).
    """
    df_type = check_row_input(df, column)
    values = frame_column(df, column)
    dtype = values.dtype
    if df_type == "polars":
        numeric = dtype.is_numeric() and values.null_count() < len(values)
    else:
        numeric = getattr(dtype, "kind", None) in ("i", "u", "f") and bool(values.notna().any())
    if numeric:
        raise TypeError(f"column {column!r}: {_PHONE_NOT_TEXT}, got dtype {dtype}")
    return df_type


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
    # None binds as NULL: the setting anofox_tab_phonenumber_default_region applies.
    return "?", [region]
