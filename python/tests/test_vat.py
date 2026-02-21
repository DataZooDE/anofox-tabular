"""
Tests for vat.py — assert return types and basic correctness.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


class TestIsValidVatCountry:
    def test_returns_bool(self, conn):
        from anofox import vat
        result = vat.is_valid_vat_country(conn, "DE")
        assert isinstance(result, bool)

    def test_de_is_valid(self, conn):
        from anofox import vat
        assert vat.is_valid_vat_country(conn, "DE") is True

    def test_fake_is_invalid(self, conn):
        from anofox import vat
        assert vat.is_valid_vat_country(conn, "ZZ") is False


class TestVatNormalize:
    def test_returns_string(self, conn):
        from anofox import vat
        result = vat.vat_normalize(conn, "de 123456789")
        assert isinstance(result, str)


class TestVatIsValidSyntax:
    def test_returns_bool(self, conn):
        from anofox import vat
        result = vat.vat_is_valid_syntax(conn, "DE123456789")
        assert isinstance(result, bool)


class TestVatSplit:
    def test_returns_dict_or_none(self, conn):
        from anofox import vat
        result = vat.vat_split(conn, "DE123456789")
        assert result is None or isinstance(result, dict)

    def test_valid_vat_splits(self, conn):
        from anofox import vat
        result = vat.vat_split(conn, "DE123456789")
        if result is not None:
            assert "country" in result or "digits" in result


class TestVatIsEuMember:
    def test_returns_bool(self, conn):
        from anofox import vat
        result = vat.vat_is_eu_member(conn, "DE")
        assert isinstance(result, bool)

    def test_de_is_eu_member(self, conn):
        from anofox import vat
        assert vat.vat_is_eu_member(conn, "DE") is True

    def test_us_is_not_eu_member(self, conn):
        from anofox import vat
        assert vat.vat_is_eu_member(conn, "US") is False


class TestVatCountryName:
    def test_returns_string(self, conn):
        from anofox import vat
        result = vat.vat_country_name(conn, "DE")
        assert isinstance(result, str)

    def test_de_name(self, conn):
        from anofox import vat
        result = vat.vat_country_name(conn, "DE")
        assert len(result) > 0


class TestVatIsValid:
    def test_returns_bool(self, conn):
        from anofox import vat
        result = vat.vat_is_valid(conn, "DE123456789")
        assert isinstance(result, bool)


class TestMakeVat:
    def test_returns_dict(self, conn):
        from anofox import vat
        result = vat.make_vat(conn, "DE123456789")
        assert isinstance(result, dict)
