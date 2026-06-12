"""
Money type wrappers.

The extension represents money as a STRUCT(amount DOUBLE, currency VARCHAR).
Python-side results come back as dicts with ``amount`` and ``currency`` keys
when DuckDB returns a struct, or as plain scalars for scalar-returning functions.
"""

from __future__ import annotations

from typing import Any


def make_money(conn: Any, amount: float, currency: str) -> dict:
    """
    Create a money value from an amount and currency code.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    amount:
        Monetary amount.
    currency:
        ISO 4217 currency code, e.g. ``"USD"`` or ``"EUR"``.

    Returns
    -------
    dict
        ``{"amount": float, "currency": str}``
    """
    result = conn.execute(
        f"SELECT anofox_tab_money({amount}, '{currency}')"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_from_cents(conn: Any, cents: int, currency: str) -> dict:
    """
    Create a money value from integer cents.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    cents:
        Amount in the smallest currency unit (e.g. cents for USD).
    currency:
        ISO 4217 currency code.

    Returns
    -------
    dict
        ``{"amount": float, "currency": str}`` where ``amount`` is in major
        units (the cents value divided by the currency's ``subunit_to_unit``,
        e.g. ``1000`` cents -> ``10.0`` USD).
    """
    result = conn.execute(
        f"SELECT anofox_tab_money_from_cents({cents}, '{currency}')"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_amount(conn: Any, money: dict) -> float:
    """Extract the numeric amount from a money dict."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_amount(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return float(result[0])


def money_currency(conn: Any, money: dict) -> str:
    """Extract the currency code from a money dict."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_currency(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return str(result[0])


def is_valid_currency(conn: Any, code: str) -> bool:
    """Return True if *code* is a known ISO 4217 currency code."""
    result = conn.execute(
        f"SELECT anofox_tab_is_valid_currency('{code}')"
    ).fetchone()
    return bool(result[0])


def currency_symbol(conn: Any, code: str) -> str:
    """Return the currency symbol for an ISO 4217 code (e.g. ``"$"`` for ``"USD"``)."""
    result = conn.execute(
        f"SELECT anofox_tab_currency_symbol('{code}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def currency_name(conn: Any, code: str) -> str:
    """Return the full name of a currency (e.g. ``"US Dollar"`` for ``"USD"``)."""
    result = conn.execute(
        f"SELECT anofox_tab_currency_name('{code}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def money_format(conn: Any, money: dict, fmt: str = "{amount} {currency}") -> str:
    """
    Format a money value as a string.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    money:
        Money dict with ``amount`` and ``currency`` keys.
    fmt:
        Format string.

    Returns
    -------
    str
    """
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_format(anofox_tab_money({amount}, '{currency}'), '{fmt}')"
    ).fetchone()
    return str(result[0]) if result[0] is not None else ""


def money_is_positive(conn: Any, money: dict) -> bool:
    """Return True if the money amount is > 0."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_is_positive(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return bool(result[0])


def money_is_negative(conn: Any, money: dict) -> bool:
    """Return True if the money amount is < 0."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_is_negative(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return bool(result[0])


def money_is_zero(conn: Any, money: dict) -> bool:
    """Return True if the money amount is 0."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_is_zero(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return bool(result[0])


def money_abs(conn: Any, money: dict) -> dict:
    """Return the absolute value of a money amount."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_abs(anofox_tab_money({amount}, '{currency}'))"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_add(conn: Any, m1: dict, m2: dict) -> dict:
    """Add two money values (must share the same currency)."""
    result = conn.execute(
        f"SELECT anofox_tab_money_add("
        f"anofox_tab_money({m1['amount']}, '{m1['currency']}'), "
        f"anofox_tab_money({m2['amount']}, '{m2['currency']}'))"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_subtract(conn: Any, m1: dict, m2: dict) -> dict:
    """Subtract *m2* from *m1* (must share the same currency)."""
    result = conn.execute(
        f"SELECT anofox_tab_money_subtract("
        f"anofox_tab_money({m1['amount']}, '{m1['currency']}'), "
        f"anofox_tab_money({m2['amount']}, '{m2['currency']}'))"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_multiply(conn: Any, money: dict, factor: float) -> dict:
    """Multiply a money amount by a scalar factor."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_multiply(anofox_tab_money({amount}, '{currency}'), {factor})"
    ).fetchone()
    return _struct_to_dict(result[0])


def money_in_range(conn: Any, money: dict, min_amount: float, max_amount: float) -> bool:
    """Return True if the money amount is within [min_amount, max_amount]."""
    amount = money.get("amount", 0)
    currency = money.get("currency", "USD")
    result = conn.execute(
        f"SELECT anofox_tab_money_in_range("
        f"anofox_tab_money({amount}, '{currency}'), {min_amount}, {max_amount})"
    ).fetchone()
    return bool(result[0])


def money_same_currency(conn: Any, m1: dict, m2: dict) -> bool:
    """Return True if *m1* and *m2* share the same currency."""
    result = conn.execute(
        f"SELECT anofox_tab_money_same_currency("
        f"anofox_tab_money({m1['amount']}, '{m1['currency']}'), "
        f"anofox_tab_money({m2['amount']}, '{m2['currency']}'))"
    ).fetchone()
    return bool(result[0])


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _struct_to_dict(value: Any) -> dict:
    """Convert a DuckDB struct value to a Python dict."""
    if isinstance(value, dict):
        return value
    # DuckDB may return structs as tuples in some driver versions
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return {"amount": float(value[0]), "currency": str(value[1])}
    return {"raw": value}
