"""
Tests for money.py — assert return types and basic arithmetic correctness.
"""

import sys
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
        assert abs(result["amount"] - 42.5) < 0.001

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
        assert abs(result["amount"] - 10.0) < 0.001

    def test_zero_decimal_currency_unchanged(self, conn):
        from anofox import money
        result = money.money_from_cents(conn, 1000, "JPY")
        # JPY has no subunit (subunit_to_unit = 1), value is unchanged
        assert abs(result["amount"] - 1000.0) < 0.001


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
        assert abs(result["amount"] - 15.0) < 0.001

    def test_subtract_returns_dict(self, conn):
        from anofox import money
        m1 = {"amount": 10.0, "currency": "EUR"}
        m2 = {"amount": 3.0, "currency": "EUR"}
        result = money.money_subtract(conn, m1, m2)
        assert isinstance(result, dict)
        assert abs(result["amount"] - 7.0) < 0.001

    def test_multiply_returns_dict(self, conn):
        from anofox import money
        m = {"amount": 10.0, "currency": "USD"}
        result = money.money_multiply(conn, m, 2.5)
        assert isinstance(result, dict)
        assert abs(result["amount"] - 25.0) < 0.001


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
