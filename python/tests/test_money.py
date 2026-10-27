"""
Tests for money.py — assert return types and exact arithmetic correctness.

Money amounts are DECIMAL(18,3) in the extension and surface as
``decimal.Decimal`` in Python (issue #57).
"""

import sys
from decimal import Decimal
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


class TestMakeMoney:
    def test_returns_dict(self, conn):
        from anofox import money
        result = money.make_money(conn, 10.0, "USD")
        assert isinstance(result, dict)

    def test_amount_field(self, conn):
        from anofox import money
        result = money.make_money(conn, 42.5, "EUR")
        assert "amount" in result
        assert isinstance(result["amount"], Decimal)
        assert result["amount"] == Decimal("42.500")

    def test_currency_field(self, conn):
        from anofox import money
        result = money.make_money(conn, 10.0, "GBP")
        assert "currency" in result
        assert result["currency"] == "GBP"


class TestMoneyFromCents:
    def test_returns_dict(self, conn):
        from anofox import money
        result = money.money_from_cents(conn, 1000, "USD")
        assert isinstance(result, dict)

    def test_converts_cents_to_dollars(self, conn):
        from anofox import money
        result = money.money_from_cents(conn, 1000, "USD")
        # 1000 cents = 10.00 USD (divided by the currency's subunit_to_unit)
        assert result["amount"] == Decimal("10.000")

    def test_zero_decimal_currency_unchanged(self, conn):
        from anofox import money
        result = money.money_from_cents(conn, 1000, "JPY")
        # JPY has no subunit (subunit_to_unit = 1), value is unchanged
        assert result["amount"] == Decimal("1000.000")


class TestIsValidCurrency:
    def test_returns_bool(self, conn):
        from anofox import money
        result = money.is_valid_currency(conn, "USD")
        assert isinstance(result, bool)

    def test_usd_is_valid(self, conn):
        from anofox import money
        assert money.is_valid_currency(conn, "USD") is True

    def test_eur_is_valid(self, conn):
        from anofox import money
        assert money.is_valid_currency(conn, "EUR") is True

    def test_fake_is_invalid(self, conn):
        from anofox import money
        assert money.is_valid_currency(conn, "ZZZ") is False


class TestCurrencySymbol:
    def test_returns_string(self, conn):
        from anofox import money
        result = money.currency_symbol(conn, "USD")
        assert isinstance(result, str)

    def test_usd_symbol(self, conn):
        from anofox import money
        result = money.currency_symbol(conn, "USD")
        assert result == "$"


class TestCurrencyName:
    def test_returns_string(self, conn):
        from anofox import money
        result = money.currency_name(conn, "USD")
        assert isinstance(result, str)

    def test_usd_name(self, conn):
        from anofox import money
        result = money.currency_name(conn, "USD")
        assert "Dollar" in result or "USD" in result


class TestMoneyArithmetic:
    def test_add_returns_dict(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "USD"}
        m2 = {"amount": 5.0, "currency": "USD"}
        result = money.money_add(conn, m1, m2)
        assert isinstance(result, dict)

    def test_add_amounts(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "USD"}
        m2 = {"amount": 5.0, "currency": "USD"}
        result = money.money_add(conn, m1, m2)
        assert result["amount"] == Decimal("15.000")

    def test_subtract_returns_dict(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "EUR"}
        m2 = {"amount": 3.0, "currency": "EUR"}
        result = money.money_subtract(conn, m1, m2)
        assert isinstance(result, dict)
        assert result["amount"] == Decimal("7.000")

    def test_multiply_returns_dict(self, conn):
        from anofox import money
        m = {"amount": 10.0, "currency": "USD"}
        result = money.money_multiply(conn, m, 2.5)
        assert isinstance(result, dict)
        assert result["amount"] == Decimal("25.000")


class TestMoneyPredicates:
    def test_is_positive_true(self, conn):
        from anofox import money
        m = {"amount": 5.0, "currency": "USD"}
        assert money.money_is_positive(conn, m) is True

    def test_is_positive_false_for_negative(self, conn):
        from anofox import money
        m = {"amount": -5.0, "currency": "USD"}
        assert money.money_is_positive(conn, m) is False

    def test_is_negative_true(self, conn):
        from anofox import money
        m = {"amount": -3.0, "currency": "USD"}
        assert money.money_is_negative(conn, m) is True

    def test_is_zero_true(self, conn):
        from anofox import money
        m = {"amount": 0.0, "currency": "USD"}
        assert money.money_is_zero(conn, m) is True

    def test_in_range_true(self, conn):
        from anofox import money
        m = {"amount": 50.0, "currency": "USD"}
        assert money.money_in_range(conn, m, 0.0, 100.0) is True

    def test_in_range_false(self, conn):
        from anofox import money
        m = {"amount": 150.0, "currency": "USD"}
        assert money.money_in_range(conn, m, 0.0, 100.0) is False

    def test_same_currency_true(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "USD"}
        m2 = {"amount": 20.0, "currency": "USD"}
        assert money.money_same_currency(conn, m1, m2) is True

    def test_same_currency_false(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "USD"}
        m2 = {"amount": 20.0, "currency": "EUR"}
        assert money.money_same_currency(conn, m1, m2) is False


class TestMoneyExactness:
    """Amounts are exact DECIMAL(18,3): no binary floating point drift (issue #57)."""

    def test_amount_is_decimal(self, conn):
        from anofox import money
        result = money.make_money(conn, 10.0, "USD")
        assert isinstance(result["amount"], Decimal)

    def test_point_one_plus_point_two_is_exactly_point_three(self, conn):
        from anofox import money
        m1 = {"amount": 0.1, "currency": "USD"}
        m2 = {"amount": 0.2, "currency": "USD"}
        result = money.money_add(conn, m1, m2)
        assert result["amount"] == Decimal("0.300")

    def test_money_amount_returns_exact_decimal(self, conn):
        from anofox import money
        amount = money.money_amount(conn, {"amount": 19.99, "currency": "USD"})
        assert isinstance(amount, Decimal)
        assert amount == Decimal("19.990")

    def test_cents_round_trip_is_exact(self, conn):
        from anofox import money
        result = money.money_from_cents(conn, 123456789, "USD")
        assert result["amount"] == Decimal("1234567.890")

    def test_nan_amount_raises(self, conn):
        import duckdb
        with pytest.raises(duckdb.Error):
            conn.execute("SELECT anofox_tab_money('NaN'::DOUBLE, 'USD')").fetchone()

    def test_overflow_raises(self, conn):
        import duckdb
        with pytest.raises(duckdb.Error):
            conn.execute("SELECT anofox_tab_money(1e16, 'USD')").fetchone()


class TestMoneyFormatting:
    """money_format derives scale and separators from currency metadata (issue #57)."""

    def test_jpy_has_no_decimals(self, conn):
        from anofox import money
        result = money.money_format(conn, {"amount": 1000, "currency": "JPY"}, "symbol")
        assert result == "\u00a51,000"

    def test_usd_thousands_separator(self, conn):
        from anofox import money
        result = money.money_format(conn, {"amount": 1234567.89, "currency": "USD"}, "symbol")
        assert result == "$1,234,567.89"


class TestMoneyFormatStyles:
    """money_format takes the SQL format styles: symbol, long, code (the default)."""

    def test_default_is_code_style(self, conn):
        from anofox import money
        m = {"amount": 1234.5, "currency": "EUR"}
        assert money.money_format(conn, m) == "1234.50 EUR"
        assert money.money_format(conn, m) == money.money_format(conn, m, "code")

    def test_long_style(self, conn):
        from anofox import money
        assert money.money_format(conn, {"amount": 1234.5, "currency": "EUR"}, "long") == "1.234,50 Euro"

    def test_default_is_documented_as_code(self):
        import inspect
        from anofox import money
        assert inspect.signature(money.money_format).parameters["fmt"].default == "code"


def _invoices():
    import pandas as pd
    return pd.DataFrame({
        "invoice_no": ["INV-2026-0141", "INV-2026-0142", "INV-2026-0143", "INV-2026-0144"],
        "total": [1249.5, 89.99, None, 15000.0],
        "currency": ["EUR", "usd", "EUR", "JPY"],
    }, index=[3, 1, 4, 2])


class TestMakeMoneyApi:
    def test_null_amount_is_none(self, conn):
        from anofox import money
        assert money.make_money(conn, None, "EUR") is None

    def test_scalar_needs_currency(self, conn):
        from anofox import money
        with pytest.raises(ValueError, match="currency"):
            money.make_money(conn, 12.5)

    def test_dataframe_currency_column(self, conn):
        import pandas as pd
        from anofox import money
        df = _invoices()
        out = money.make_money(conn, df, column="total", currency_column="currency")
        assert out.index.tolist() == [3, 1, 4, 2]
        assert out["invoice_no"].tolist() == df["invoice_no"].tolist()
        values = out["make_money"].tolist()
        assert values[0] == {"amount": Decimal("1249.500"), "currency": "EUR"}
        assert values[1] == {"amount": Decimal("89.990"), "currency": "USD"}
        assert pd.isna(values[2])  # missing amount -> missing money value
        assert values[3] == {"amount": Decimal("15000.000"), "currency": "JPY"}

    def test_dataframe_one_currency(self, conn):
        from anofox import money
        out = money.make_money(conn, _invoices().head(2), "EUR", column="total")
        assert [v["currency"] for v in out["make_money"]] == ["EUR", "EUR"]
        assert out["make_money"].tolist()[1]["amount"] == Decimal("89.990")

    def test_currency_and_currency_column_conflict(self, conn):
        from anofox import money
        with pytest.raises(ValueError, match="currency_column"):
            money.make_money(conn, _invoices(), "EUR", column="total", currency_column="currency")

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import money
        out = money.make_money(conn, pl.from_pandas(_invoices()), column="total", currency_column="currency")
        assert isinstance(out, pl.DataFrame)
        assert out["make_money"].to_list()[2] is None

    def test_series_is_a_type_error(self, conn):
        import pandas as pd
        from anofox import money
        with pytest.raises(TypeError, match=r"Series.*to_frame"):
            money.make_money(conn, pd.Series([12.5], name="total"), "EUR")


class TestMakeMoneyBadRows:
    """On a DataFrame, a row the extension rejects gives a missing value (python/README.md)."""

    def test_unknown_currency_code_gives_none(self, conn):
        import pandas as pd
        from anofox import money
        df = pd.DataFrame({
            "invoice_no": ["INV-2026-0201", "INV-2026-0202", "INV-2026-0203"],
            "total": [99.5, 12.0, 450.25],
            "currency": ["EUR", "EUR?", "chf"],
        })
        out = money.make_money(conn, df, column="total", currency_column="currency")
        values = out["make_money"].tolist()
        assert values[0] == {"amount": Decimal("99.500"), "currency": "EUR"}
        assert pd.isna(values[1])
        assert values[2] == {"amount": Decimal("450.250"), "currency": "CHF"}

    def test_polars_nan_amount_gives_none(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import money
        df = pl.DataFrame({"total": [12.5, float("nan"), None]})
        out = money.make_money(conn, df, "USD", column="total")
        assert out["make_money"].to_list() == [
            {"amount": Decimal("12.500"), "currency": "USD"}, None, None,
        ]

    def test_scalar_unknown_currency_still_raises(self, conn):
        import duckdb
        from anofox import money
        with pytest.raises(duckdb.Error, match="currency"):
            money.make_money(conn, 10.0, "EUR?")


class TestMoneyFromCentsRange:
    def test_beyond_bigint_raises_value_error(self, conn):
        from anofox import money
        with pytest.raises(ValueError, match="BIGINT"):
            money.money_from_cents(conn, 2**63, "EUR")
        with pytest.raises(ValueError, match="BIGINT"):
            money.money_from_cents(conn, -(2**63) - 1, "EUR")

    def test_largest_amount_still_works(self, conn):
        import numpy as np
        from anofox import money
        largest = money.money_from_cents(conn, np.int64(99_999_999_999_999_999), "USD")
        assert largest == {"amount": Decimal("999999999999999.99"), "currency": "USD"}
