"""
Money type wrappers.

The extension represents money as a STRUCT(amount DECIMAL(18,3), currency VARCHAR).
Amounts are exact decimals: DuckDB returns them as :class:`decimal.Decimal`, and
the wrappers preserve that type so no binary floating point drift is introduced.
Python-side results come back as dicts with ``amount`` and ``currency`` keys
when DuckDB returns a struct, or as plain scalars for scalar-returning functions.
"""

from __future__ import annotations

from decimal import Decimal
from typing import Any, Optional, Union

from ._sql import fetch_one, fetch_row, quote_ident
from ._table import apply_scalar, check_row_input


def make_money(
    conn: Any,
    amount: Union[float, int, Decimal, None, Any],
    currency: Optional[str] = None,
    column: Optional[str] = None,
    currency_column: Optional[str] = None,
) -> Union[Optional[dict], Any]:
    """
    Create a money value from an amount and currency code, like SQL ``money(amount, currency)``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    amount:
        Monetary amount, or a pandas/polars DataFrame (with *column*). A single
        amount must be finite (NaN/Inf raise) and within the DECIMAL(18,3) range; it is
        rounded half away from zero to 3 decimals. A :class:`decimal.Decimal`
        is kept exact; a ``float`` holds about 15 significant digits. ``None``
        gives ``None``.
    currency:
        ISO 4217 currency code for every value, e.g. ``"USD"`` or ``"EUR"``
        (case-insensitive; stored uppercase). Required for a single amount.
        For a single amount an unknown code raises a DuckDB error.
    column:
        DataFrame only: column holding the amounts. A missing amount (``None``
        or NaN) gives a missing money value, and so does a row the extension
        rejects (unknown currency code, infinite or out-of-range amount):
        a DataFrame never raises for a bad row.
    currency_column:
        DataFrame only: column holding the currency code of each row. Cannot
        be combined with *currency*.

    Returns
    -------
    dict or None
        ``{"amount": decimal.Decimal, "currency": str}`` for a single amount.
    DataFrame
        For a DataFrame: a copy with an added ``make_money`` column of such
        dicts (input index and row order kept).
    """
    if currency is not None and currency_column is not None:
        raise ValueError("pass either 'currency' or 'currency_column', not both.")
    if amount is None or (isinstance(amount, (int, float, Decimal)) and not isinstance(amount, bool)):
        if currency_column is not None:
            raise ValueError("'currency_column' requires a DataFrame input.")
        if currency is None:
            raise ValueError("'currency' is required for a single amount.")
        raw = fetch_one(conn, "SELECT anofox_tab_money(?, ?)", [amount, currency])
        return None if raw is None else _struct_to_dict(raw)

    df_type = check_row_input(amount, column)
    if currency_column is not None:
        currency_sql, params = quote_ident(currency_column), []
    elif currency is not None:
        currency_sql, params = "?", [currency]
    else:
        raise ValueError("pass 'currency' or 'currency_column' with a DataFrame.")
    return apply_scalar(
        conn, amount, df_type,
        # TRY: a row the extension rejects (unknown currency code, NaN/Inf or
        # out-of-range amount) becomes a missing value instead of failing the
        # whole DataFrame, like every other row-wise wrapper.
        f"TRY(anofox_tab_money({quote_ident(column)}, {currency_sql}))", params, "make_money",
    )


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
        ``{"amount": decimal.Decimal, "currency": str}`` where ``amount`` is in
        major units (the cents value divided by the currency's
        ``subunit_to_unit``, e.g. ``1000`` cents -> ``Decimal("10.000")`` USD).
        The conversion is exact for the full BIGINT cents range.
    """
    result = fetch_row(
        conn,
        "SELECT anofox_tab_money_from_cents(?, ?)", [cents, currency]
    )
    return _struct_to_dict(result[0])


def money_amount(conn: Any, money: dict) -> Decimal:
    """Extract the exact numeric amount (``decimal.Decimal``) from a money dict."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_amount({_MONEY_SQL})", _money_params(money)
    )
    return Decimal(result[0]) if not isinstance(result[0], Decimal) else result[0]


def money_currency(conn: Any, money: dict) -> str:
    """Extract the currency code from a money dict."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_currency({_MONEY_SQL})", _money_params(money)
    )
    return str(result[0])


def is_valid_currency(conn: Any, code: str) -> bool:
    """Return True if *code* is a known ISO 4217 currency code."""
    result = fetch_row(
        conn,
        "SELECT anofox_tab_is_valid_currency(?)", [code]
    )
    return bool(result[0])


def currency_symbol(conn: Any, code: str) -> str:
    """Return the currency symbol for an ISO 4217 code (e.g. ``"$"`` for ``"USD"``); ``""`` for an unsupported code."""
    result = fetch_row(
        conn,
        "SELECT anofox_tab_currency_symbol(?)", [code]
    )
    return str(result[0]) if result[0] is not None else ""


def currency_name(conn: Any, code: str) -> str:
    """Return the full name of a currency (e.g. ``"United States Dollar"`` for ``"USD"``); ``""`` for an unsupported code."""
    result = fetch_row(
        conn,
        "SELECT anofox_tab_currency_name(?)", [code]
    )
    return str(result[0]) if result[0] is not None else ""


def money_format(conn: Any, money: dict, fmt: str = "code") -> str:
    """
    Format a money value as a string, like SQL ``money_format(money, format_style)``.

    Parameters
    ----------
    conn:
        An :class:`~anofox.AnofoxConnection`.
    money:
        Money dict with ``amount`` and ``currency`` keys.
    fmt:
        Format style: ``"code"`` (default, ``1234.50 EUR``), ``"symbol"``
        (``1.234,50 €``) or ``"long"`` (``1.234,50 Euro``), case-insensitive.
        Any other string raises a DuckDB error; it is not a template.

    Returns
    -------
    str
    """
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_format({_MONEY_SQL}, ?)", [*_money_params(money), fmt]
    )
    return str(result[0]) if result[0] is not None else ""


def money_is_positive(conn: Any, money: dict) -> bool:
    """Return True if the money amount is > 0."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_is_positive({_MONEY_SQL})", _money_params(money)
    )
    return bool(result[0])


def money_is_negative(conn: Any, money: dict) -> bool:
    """Return True if the money amount is < 0."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_is_negative({_MONEY_SQL})", _money_params(money)
    )
    return bool(result[0])


def money_is_zero(conn: Any, money: dict) -> bool:
    """Return True if the money amount is 0."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_is_zero({_MONEY_SQL})", _money_params(money)
    )
    return bool(result[0])


def money_abs(conn: Any, money: dict) -> dict:
    """Return the absolute value of a money amount."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_abs({_MONEY_SQL})", _money_params(money)
    )
    return _struct_to_dict(result[0])


def money_add(conn: Any, m1: dict, m2: dict) -> dict:
    """Add two money values (must share the same currency)."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_add({_MONEY_SQL}, {_MONEY_SQL})",
        [*_money_params(m1), *_money_params(m2)],
    )
    return _struct_to_dict(result[0])


def money_subtract(conn: Any, m1: dict, m2: dict) -> dict:
    """Subtract *m2* from *m1* (must share the same currency)."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_subtract({_MONEY_SQL}, {_MONEY_SQL})",
        [*_money_params(m1), *_money_params(m2)],
    )
    return _struct_to_dict(result[0])


def money_multiply(conn: Any, money: dict, factor: float) -> dict:
    """Multiply a money amount by a scalar factor."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_multiply({_MONEY_SQL}, ?)", [*_money_params(money), factor]
    )
    return _struct_to_dict(result[0])


def money_in_range(conn: Any, money: dict, min_amount: float, max_amount: float) -> bool:
    """Return True if the money amount is within [min_amount, max_amount]."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_in_range({_MONEY_SQL}, ?, ?)",
        [*_money_params(money), min_amount, max_amount],
    )
    return bool(result[0])


def money_same_currency(conn: Any, m1: dict, m2: dict) -> bool:
    """Return True if *m1* and *m2* share the same currency."""
    result = fetch_row(
        conn,
        f"SELECT anofox_tab_money_same_currency({_MONEY_SQL}, {_MONEY_SQL})",
        [*_money_params(m1), *_money_params(m2)],
    )
    return bool(result[0])


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

_MONEY_SQL = "anofox_tab_money(?, ?)"


def _money_params(money: dict) -> list:
    """Bound parameters (amount, currency) that rebuild *money* through ``anofox_tab_money``."""
    return [money.get("amount", 0), money.get("currency", "USD")]


def _struct_to_dict(value: Any) -> dict:
    """Convert a DuckDB struct value to a Python dict (amount stays an exact Decimal)."""
    if isinstance(value, dict):
        return value
    # DuckDB may return structs as tuples in some driver versions
    if isinstance(value, (list, tuple)) and len(value) == 2:
        amount = value[0] if isinstance(value[0], Decimal) else Decimal(str(value[0]))
        return {"amount": amount, "currency": str(value[1])}
    return {"raw": value}
