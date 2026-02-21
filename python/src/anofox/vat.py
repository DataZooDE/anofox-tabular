"""
VAT number validation and lookup wrappers.
"""

from __future__ import annotations

from typing import Any, Optional


def make_vat(conn: Any, vat_string: str) -> dict:
    """
    Parse a VAT number string into a structured VAT value.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    vat_string:
        VAT number including country prefix, e.g. ``"DE123456789"``.

    Returns
    -------
    dict
        ``{"country": str, "digits": str}``
    """
    result = conn.execute(
        f"SELECT anofox_tab_vat('{vat_string}')"
    ).fetchone()
    raw = result[0]
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, (list, tuple)) and len(raw) == 2:
        return {"country": str(raw[0]), "digits": str(raw[1])}
    return {"raw": raw}


def is_valid_vat_country(conn: Any, country_code: str) -> bool:
    """Return True if *country_code* is a supported VAT country."""
    result = conn.execute(
        f"SELECT anofox_tab_is_valid_vat_country('{country_code}')"
    ).fetchone()
    return bool(result[0])


def vat_normalize(conn: Any, vat_string: str) -> str:
    """Normalize a VAT number to a canonical format."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_normalize('{vat_string}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def vat_is_valid_syntax(conn: Any, vat_string: str) -> bool:
    """
    Return True if *vat_string* passes country-specific VAT syntax validation.

    *vat_string* should be the full VAT number including country prefix,
    e.g. ``'DE123456789'``.
    """
    result = conn.execute(
        f"SELECT anofox_tab_vat_is_valid_syntax('{vat_string}')"
    ).fetchone()
    return bool(result[0])


def vat_split(conn: Any, vat_string: str) -> Optional[dict]:
    """
    Split a VAT string into country code and digit parts.

    Returns *None* if the VAT string cannot be parsed.
    """
    result = conn.execute(
        f"SELECT anofox_tab_vat_split('{vat_string}')"
    ).fetchone()
    raw = result[0]
    if raw is None:
        return None
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, (list, tuple)) and len(raw) == 2:
        return {"country": str(raw[0]), "digits": str(raw[1])}
    return {"raw": raw}


def vat_exists(conn: Any, country_code: str) -> bool:
    """Return True if *country_code* has VAT registration."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_exists('{country_code}')"
    ).fetchone()
    return bool(result[0])


def vat_is_eu_member(conn: Any, country_code: str) -> bool:
    """Return True if *country_code* is an EU member state."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_is_eu_member('{country_code}')"
    ).fetchone()
    return bool(result[0])


def vat_country_name(conn: Any, country_code: str) -> str:
    """Return the full country name for a VAT country code."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_country_name('{country_code}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def vat_format(conn: Any, country: str, digits: str) -> str:
    """Format a country + digits pair as a canonical VAT string."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_format('{country}', '{digits}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def vat_is_valid(conn: Any, vat_string: str) -> bool:
    """Return True if *vat_string* is a syntactically valid VAT number."""
    result = conn.execute(
        f"SELECT anofox_tab_vat_is_valid('{vat_string}')"
    ).fetchone()
    return bool(result[0])
