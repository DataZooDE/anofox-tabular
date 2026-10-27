"""
VAT number validation and lookup wrappers.

Every function takes a single string (returns a scalar) or a pandas/polars
DataFrame with ``column=`` (returns a copy of the DataFrame with one added
column named after the function, input index and row order kept). A value
the extension cannot parse gives ``None`` (SQL ``NULL``). The checks run
locally; none of them queries the EU VIES service.
"""

from __future__ import annotations

from typing import Any, Callable, Optional, Union

from ._sql import fetch_one, quote_ident
from ._table import apply_scalar, check_row_input


def make_vat(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[Optional[dict], Any]:
    """
    Parse a VAT number into ``{"country": str, "digits": str}``, like SQL ``vat(vat_number)``.

    *vat_string* is a VAT number including its prefix, e.g. ``"DE123456789"``,
    or a DataFrame (with *column*). Returns ``None`` when the number cannot be
    parsed.
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat", "make_vat", _to_vat_dict)


def vat_split(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[Optional[dict], Any]:
    """
    Split a VAT number into ``{"country": str, "digits": str}``.

    Returns ``None`` if the VAT number cannot be parsed. Accepts a DataFrame
    with *column*.
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat_split", "vat_split", _to_vat_dict)


def vat_is_valid(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[bool, Any]:
    """
    Return True if *vat_string* is a valid VAT number: supported prefix,
    country syntax and, where the country has them, check digits.

    Accepts a DataFrame with *column*.
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat_is_valid", "vat_is_valid", _to_bool)


def vat_is_valid_syntax(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[bool, Any]:
    """
    Return True if *vat_string* passes the country-specific VAT syntax check
    (prefix and number pattern, no check digits).

    *vat_string* is the full VAT number including its prefix, e.g.
    ``'DE123456789'``, or a DataFrame (with *column*).
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat_is_valid_syntax", "vat_is_valid_syntax", _to_bool)


def vat_exists(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[bool, Any]:
    """
    Return True if *vat_string* starts with a supported VAT prefix followed by
    a number, like SQL ``vat_exists(vat_number)``.

    Only the prefix is checked: not the length or pattern of the number, and
    not whether the number is registered (nothing is looked up online). For
    example ``"DE12"`` is True, while ``"DE"`` (no number after the prefix)
    and ``"UK123456789"`` are False (``UK`` is not a VAT prefix; use ``GB`` or
    ``XI``). Accepts a DataFrame with *column*.
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat_exists", "vat_exists", _to_bool)


def vat_normalize(conn: Any, vat_string: Union[str, Any], column: Optional[str] = None) -> Union[Optional[str], Any]:
    """
    Normalize a VAT number: uppercase, separators removed (``"de 111-111-125"``
    becomes ``"DE111111125"``). Accepts a DataFrame with *column*.
    """
    return _vat_call(conn, vat_string, column, "anofox_tab_vat_normalize", "vat_normalize", _to_str)


def vat_format(
    conn: Any, vat: Union[str, Any], style: str = "plain", column: Optional[str] = None
) -> Union[Optional[str], Any]:
    """
    Format a VAT number, like SQL ``vat_format(vat_number, format_style)``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    vat:
        VAT number in any common spelling, e.g. ``"de 111-111-125"``, or a
        DataFrame (with *column*).
    style:
        ``"plain"`` (default): the national part without the country prefix,
        letters included (``"U13585627"`` for ``"ATU13585627"``); ``"iso"``:
        VAT prefix and national part, e.g. ``"DE111111125"`` (Greece as
        ``EL``). Neither style validates the number.
        Case-insensitive; an unknown style raises a DuckDB error.
    column:
        Column name when a DataFrame is passed.

    Returns
    -------
    str or None
        *None* when *vat* cannot be parsed.
    """
    return _vat_call(conn, vat, column, "anofox_tab_vat_format", "vat_format", _to_str, [style])


def is_valid_vat_country(conn: Any, country_code: Union[str, Any], column: Optional[str] = None) -> Union[bool, Any]:
    """Return True if *country_code* (e.g. ``"DE"``) is a supported VAT prefix. Accepts a DataFrame with *column*."""
    return _vat_call(conn, country_code, column, "anofox_tab_is_valid_vat_country", "is_valid_vat_country", _to_bool)


def vat_is_eu_member(conn: Any, country_code: Union[str, Any], column: Optional[str] = None) -> Union[bool, Any]:
    """Return True if *country_code* is an EU member state. Accepts a DataFrame with *column*."""
    return _vat_call(conn, country_code, column, "anofox_tab_vat_is_eu_member", "vat_is_eu_member", _to_bool)


def vat_country_name(conn: Any, country_code: Union[str, Any], column: Optional[str] = None) -> Union[Optional[str], Any]:
    """
    Return the country name of a VAT country code (``"DE"`` gives
    ``"Germany"``), or ``None`` for an unknown code. Accepts a DataFrame with
    *column*.
    """
    return _vat_call(conn, country_code, column, "anofox_tab_vat_country_name", "vat_country_name", _to_str)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _vat_call(
    conn: Any,
    value: Union[str, Any],
    column: Optional[str],
    sql_function: str,
    result_name: str,
    convert: Callable[[Any], Any],
    extra_params: Optional[list] = None,
) -> Any:
    """Call *sql_function* on a string (converted with *convert*) or on *column* of a DataFrame."""
    extra = list(extra_params or [])
    placeholders = "".join(", ?" for _ in extra)
    if isinstance(value, str):
        return convert(fetch_one(conn, f"SELECT {sql_function}(?{placeholders})", [value, *extra]))

    df_type = check_row_input(value, column)
    return apply_scalar(
        conn, value, df_type,
        f"{sql_function}({quote_ident(column)}{placeholders})", extra, result_name,
    )


def _to_bool(raw: Any) -> Optional[bool]:
    return None if raw is None else bool(raw)


def _to_str(raw: Any) -> Optional[str]:
    return None if raw is None else str(raw)


def _to_vat_dict(raw: Any) -> Optional[dict]:
    if raw is None or isinstance(raw, dict):
        return raw
    if isinstance(raw, (list, tuple)) and len(raw) == 2:
        return {"country": str(raw[0]), "digits": str(raw[1])}
    return {"raw": raw}
