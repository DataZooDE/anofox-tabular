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


class TestVatFormat:
    """vat_format mirrors SQL vat_format(vat_number, format_style)."""

    def test_default_style_is_plain(self, conn):
        from anofox.vat import vat_format
        assert vat_format(conn, "de 111-111-125") == "111111125"

    def test_iso_style(self, conn):
        from anofox.vat import vat_format
        assert vat_format(conn, "de 111-111-125", style="iso") == "DE111111125"
        assert vat_format(conn, "GR123456783", "iso") == "EL123456783"

    def test_unparseable_returns_none(self, conn):
        from anofox.vat import vat_format
        assert vat_format(conn, "not a vat", "iso") is None


def _suppliers():
    import pandas as pd
    return pd.DataFrame({
        "supplier": ["Brenner GmbH", "Kowalski Sp. z o.o.", "Acme UK Ltd", "Typo AG"],
        "vat_id": ["de 111-111-125", "DE123456789", "UK123456789", "DE12"],
        "country": ["DE", "DE", "UK", "DE"],
    }, index=[40, 10, 30, 20])


class TestVatInvalidInputIsNone:
    def test_country_name_unknown(self, conn):
        from anofox import vat
        assert vat.vat_country_name(conn, "ZZ") is None

    def test_make_vat_unparsable(self, conn):
        from anofox import vat
        assert vat.make_vat(conn, "UK123456789") is None

    def test_vat_format_unparsable(self, conn):
        from anofox import vat
        assert vat.vat_format(conn, "garbage", "iso") is None


class TestVatExists:
    def test_checks_the_prefix_of_a_vat_number(self, conn):
        from anofox import vat
        assert vat.vat_exists(conn, "DE111111125") is True
        assert vat.vat_exists(conn, "DE12") is True        # pattern is not checked
        assert vat.vat_exists(conn, "UK123456789") is False  # UK is not a VAT prefix

    def test_keyword_is_vat_string(self, conn):
        from anofox import vat
        assert vat.vat_exists(conn, vat_string="EL123456783") is True

    def test_docstring_says_prefix(self):
        from anofox import vat
        doc = vat.vat_exists.__doc__
        assert "prefix" in doc and "registered" in doc


VAT_NUMBER_FUNCS = [
    ("vat_is_valid", [True, False, False, False]),
    ("vat_is_valid_syntax", [True, True, False, False]),
    ("vat_exists", [True, True, False, True]),
    ("vat_normalize", ["DE111111125", "DE123456789", "UK123456789", "DE12"]),
    ("vat_format", ["111111125", "123456789", None, "12"]),
    ("vat_split", [{"country": "DE", "digits": "111111125"}, {"country": "DE", "digits": "123456789"},
                   None, {"country": "DE", "digits": "12"}]),
    ("make_vat", [{"country": "DE", "digits": "111111125"}, {"country": "DE", "digits": "123456789"},
                  None, {"country": "DE", "digits": "12"}]),
]

COUNTRY_FUNCS = [
    ("is_valid_vat_country", [True, True, False, True]),
    ("vat_is_eu_member", [True, True, False, True]),
    ("vat_country_name", ["Germany", "Germany", None, "Germany"]),
]


def _values(series):
    """List of *series* with NaN/NA/None as None."""
    import pandas as pd
    return [None if not isinstance(v, dict) and pd.isna(v) else v for v in series.tolist()]


class TestVatDataFrame:
    @pytest.mark.parametrize("name,expected", VAT_NUMBER_FUNCS)
    def test_vat_number_column(self, conn, name, expected):
        from anofox import vat
        df = _suppliers()
        out = getattr(vat, name)(conn, df, column="vat_id")
        assert out.index.tolist() == [40, 10, 30, 20]
        assert out["vat_id"].tolist() == df["vat_id"].tolist()
        assert _values(out[name]) == expected

    @pytest.mark.parametrize("name,expected", COUNTRY_FUNCS)
    def test_country_column(self, conn, name, expected):
        from anofox import vat
        out = getattr(vat, name)(conn, _suppliers(), column="country")
        assert _values(out[name]) == expected

    def test_vat_format_style(self, conn):
        from anofox import vat
        out = vat.vat_format(conn, _suppliers(), "iso", column="vat_id")
        assert _values(out["vat_format"])[:2] == ["DE111111125", "DE123456789"]

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import vat
        out = vat.vat_is_valid(conn, pl.from_pandas(_suppliers()), column="vat_id")
        assert isinstance(out, pl.DataFrame)
        assert out["vat_is_valid"].to_list() == [True, False, False, False]
