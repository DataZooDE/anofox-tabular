"""
Tests for validate.py — assert Python API contract, not SQL correctness.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


class TestEmailIsValid:
    def test_returns_bool_for_string(self, conn):
        from anofox.validate import email_is_valid
        result = email_is_valid(conn, "test@example.com")
        assert isinstance(result, bool)

    def test_valid_email_returns_true(self, conn):
        from anofox.validate import email_is_valid
        assert email_is_valid(conn, "valid@example.com", mode="regex") is True

    def test_invalid_email_returns_false(self, conn):
        from anofox.validate import email_is_valid
        assert email_is_valid(conn, "not-an-email", mode="regex") is False

    def test_unknown_dns_outcome_is_none(self, conn):
        import uuid
        from anofox.validate import email_is_valid
        # A 1 ms DNS budget and an uncached random domain: the lookup cannot finish
        conn.execute("SET anofox_tab_email_dns_timeout_ms = 1")
        try:
            assert email_is_valid(conn, f"lena@{uuid.uuid4().hex}-anofox.com", mode="dns") is None
        finally:
            conn.execute("RESET anofox_tab_email_dns_timeout_ms")

    def test_mode_is_forwarded(self, conn):
        from anofox.validate import email_is_valid
        # Just check it doesn't raise — mode forwarding is verified by function executing
        result = email_is_valid(conn, "test@example.com", mode="regex")
        assert isinstance(result, bool)

    def test_dataframe_returns_dataframe(self, conn):
        import pandas as pd
        from anofox.validate import email_is_valid
        df = pd.DataFrame({"email": ["test@example.com", "bad-email"]})
        result = email_is_valid(conn, df, column="email")
        assert isinstance(result, pd.DataFrame)
        assert "email_is_valid" in result.columns
        assert len(result) == 2

    def test_dataframe_has_bool_column(self, conn):
        import pandas as pd
        from anofox.validate import email_is_valid
        df = pd.DataFrame({"email": ["test@example.com", "bad"]})
        result = email_is_valid(conn, df, column="email")
        assert result["email_is_valid"].dtype == bool or str(result["email_is_valid"].dtype) in ("bool", "boolean")

    def test_raises_when_df_without_column(self, conn):
        import pandas as pd
        from anofox.validate import email_is_valid
        df = pd.DataFrame({"email": ["test@example.com"]})
        with pytest.raises(ValueError, match="'column' is required"):
            email_is_valid(conn, df)

    def test_polars_roundtrip(self, conn):
        polars = pytest.importorskip("polars")
        from anofox.validate import email_is_valid
        df = polars.DataFrame({"email": ["test@example.com"]})
        result = email_is_valid(conn, df, column="email")
        assert isinstance(result, polars.DataFrame)
        assert "email_is_valid" in result.columns

    def test_pandas_preserves_original_columns(self, conn):
        import pandas as pd
        from anofox.validate import email_is_valid
        df = pd.DataFrame({"id": [1, 2], "email": ["a@b.com", "bad"]})
        result = email_is_valid(conn, df, column="email")
        assert "id" in result.columns
        assert "email" in result.columns
        assert "email_is_valid" in result.columns

    def test_dataframe_temp_table_is_dropped(self, conn):
        import pandas as pd
        from anofox.validate import email_is_valid

        before = conn.execute(
            "SELECT count(*) FROM information_schema.tables "
            "WHERE table_name LIKE '_anofox_%'"
        ).fetchone()[0]
        df = pd.DataFrame({"email": ["test@example.com", "bad"]})
        email_is_valid(conn, df, column="email")
        after = conn.execute(
            "SELECT count(*) FROM information_schema.tables "
            "WHERE table_name LIKE '_anofox_%'"
        ).fetchone()[0]
        assert after == before


class TestEmailValidate:
    def test_returns_dict_for_string(self, conn):
        from anofox.validate import email_validate
        result = email_validate(conn, "test@example.com")
        assert isinstance(result, dict)

    def test_dataframe_returns_dataframe(self, conn):
        import pandas as pd
        from anofox.validate import email_validate
        df = pd.DataFrame({"email": ["test@example.com"]})
        result = email_validate(conn, df, column="email")
        assert isinstance(result, pd.DataFrame)
        assert "email_validate" in result.columns

    def test_raises_when_df_without_column(self, conn):
        import pandas as pd
        from anofox.validate import email_validate
        df = pd.DataFrame({"email": ["test@example.com"]})
        with pytest.raises(ValueError, match="'column' is required"):
            email_validate(conn, df)


class TestPhoneIsValid:
    def test_returns_bool_for_string(self, conn):
        from anofox.validate import phone_is_valid
        result = phone_is_valid(conn, "+14155552671", region="US")
        assert isinstance(result, bool)

    def test_valid_us_number(self, conn):
        from anofox.validate import phone_is_valid
        assert phone_is_valid(conn, "+14155552671", region="US") is True

    def test_dataframe_returns_dataframe(self, conn):
        import pandas as pd
        from anofox.validate import phone_is_valid
        df = pd.DataFrame({"phone": ["+14155552671", "123"]})
        result = phone_is_valid(conn, df, column="phone")
        assert isinstance(result, pd.DataFrame)
        assert "phone_is_valid" in result.columns

    def test_raises_when_df_without_column(self, conn):
        import pandas as pd
        from anofox.validate import phone_is_valid
        df = pd.DataFrame({"phone": ["+14155552671"]})
        with pytest.raises(ValueError, match="'column' is required"):
            phone_is_valid(conn, df)


class TestPhoneFormat:
    def test_returns_string(self, conn):
        from anofox.validate import phone_format
        result = phone_format(conn, "+14155552671", "US", "E164")
        assert isinstance(result, str)

    def test_e164_format(self, conn):
        from anofox.validate import phone_format
        result = phone_format(conn, "+14155552671", "US", "E164")
        assert result.startswith("+")


class TestPhoneRegion:
    def test_returns_string(self, conn):
        from anofox.validate import phone_region
        result = phone_region(conn, "+14155552671", "US")
        assert isinstance(result, str)

    def test_us_number_returns_us(self, conn):
        from anofox.validate import phone_region
        result = phone_region(conn, "+14155552671", "US")
        assert result == "US"


class TestPhoneRegionColumn:
    """region_column= reads the region hint per row instead of one region for all rows."""

    @staticmethod
    def _df():
        import pandas as pd
        return pd.DataFrame({
            "phone": ["4155552671", "01 42 68 53 00", "4155552671"],
            "country": ["US", "FR", "FR"],
        })

    def test_phone_is_valid_per_row_region(self, conn):
        from anofox.validate import phone_is_valid
        out = phone_is_valid(conn, self._df(), column="phone", region_column="country")
        assert out["phone_is_valid"].tolist() == [True, True, False]

    def test_phone_parse_per_row_region(self, conn):
        from anofox.validate import phone_parse
        out = phone_parse(conn, self._df(), column="phone", region_column="country")
        # region is the region the number was read in, also when it is invalid there
        assert [r["region"] for r in out["phone_parse"]] == ["US", "FR", "FR"]
        assert [r["valid"] for r in out["phone_parse"]] == [True, True, False]

    def test_polars_per_row_region(self, conn):
        pl = pytest.importorskip("polars")
        from anofox.validate import phone_is_valid
        out = phone_is_valid(conn, pl.from_pandas(self._df()), column="phone", region_column="country")
        assert out["phone_is_valid"].to_list() == [True, True, False]

    def test_region_and_region_column_are_exclusive(self, conn):
        from anofox.validate import phone_is_valid, phone_parse
        for func in (phone_is_valid, phone_parse):
            with pytest.raises(ValueError, match="region.*region_column"):
                func(conn, self._df(), column="phone", region="US", region_column="country")

    def test_region_column_needs_a_dataframe(self, conn):
        from anofox.validate import phone_is_valid
        with pytest.raises(ValueError, match="region_column"):
            phone_is_valid(conn, "4155552671", region_column="country")

    def test_default_region_is_us(self, conn):
        from anofox.validate import phone_is_valid
        out = phone_is_valid(conn, self._df(), column="phone")
        assert out["phone_is_valid"].tolist() == [True, False, True]


def _customers():
    import pandas as pd
    return pd.DataFrame({
        "customer_id": [1001, 1002, 1003, 1004],
        "phone": ["030 901820", "(650) 253-0000", "01 42 68 53 00", "12"],
        "country": ["DE", "US", "FR", "DE"],
    }, index=[11, 7, 3, 5])


class _DefaultRegion:
    """Set anofox_tab_phonenumber_default_region for the block, then reset it."""

    def __init__(self, conn, region):
        self.conn, self.region = conn, region

    def __enter__(self):
        self.conn.execute(f"SET anofox_tab_phonenumber_default_region = '{self.region}'")
        self.conn.complete()

    def __exit__(self, *exc):
        self.conn.execute("RESET anofox_tab_phonenumber_default_region")
        self.conn.complete()


class TestPhoneFormatApi:
    def test_unparsable_number_is_none(self, conn):
        from anofox.validate import phone_format
        assert phone_format(conn, "12", "DE") is None

    def test_region_and_format(self, conn):
        from anofox.validate import phone_format
        assert phone_format(conn, "030 901820", "DE", "INTERNATIONAL") == "+49 30 901820"
        assert phone_format(conn, "030 901820", region="DE", format="RFC3966") == "tel:+49-30-901820"

    def test_default_region_setting_applies(self, conn):
        from anofox.validate import phone_format, phone_is_valid, phone_region
        with _DefaultRegion(conn, "DE"):
            assert phone_format(conn, "030 901820") == "+4930901820"
            assert phone_is_valid(conn, "030 901820") is True
            assert phone_region(conn, "030 901820") == "DE"

    def test_dataframe_region_column(self, conn):
        from anofox.validate import phone_format
        df = _customers()
        out = phone_format(conn, df, format="INTERNATIONAL", column="phone", region_column="country")
        assert out.index.tolist() == [11, 7, 3, 5]
        assert out["phone_format"].tolist()[:3] == ["+49 30 901820", "+1 650 253 0000", "+33 1 42 68 53 00"]
        assert out["phone_format"].isna().tolist() == [False, False, False, True]
        assert out["phone"].tolist() == df["phone"].tolist()

    def test_dataframe_one_region(self, conn):
        from anofox.validate import phone_format
        df = _customers().head(1)
        out = phone_format(conn, df, region="DE", column="phone")
        assert out["phone_format"].tolist() == ["+4930901820"]

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox.validate import phone_format
        df = pl.from_pandas(_customers())
        out = phone_format(conn, df, column="phone", region_column="country")
        assert isinstance(out, pl.DataFrame)
        assert out["phone_format"].to_list() == ["+4930901820", "+16502530000", "+33142685300", None]


class TestPhoneRegionApi:
    def test_unparsable_number_is_none(self, conn):
        from anofox.validate import phone_region
        assert phone_region(conn, "12", "DE") is None

    def test_dataframe(self, conn):
        from anofox.validate import phone_region
        out = phone_region(conn, _customers(), column="phone", region_column="country")
        assert out.index.tolist() == [11, 7, 3, 5]
        assert out["phone_region"].tolist()[:3] == ["DE", "US", "FR"]
        assert out["phone_region"].isna().tolist()[3]


class TestPhoneFormatKeyword:
    """phone_format takes format=; fmt= still works but is deprecated."""

    def test_format_keyword(self, conn):
        from anofox import validate
        assert validate.phone_format(conn, "+49 30 901820", region="DE", format="NATIONAL") == "030 901820"

    def test_format_keyword_on_dataframe(self, conn):
        import pandas as pd
        from anofox import validate
        crm = pd.DataFrame({"phone": ["+49 30 901820", "+33 1 42 68 53 00"]}, index=["C-17", "C-18"])
        out = validate.phone_format(conn, crm, column="phone", format="INTERNATIONAL")
        assert out["phone_format"].tolist() == ["+49 30 901820", "+33 1 42 68 53 00"]

    def test_fmt_is_deprecated_alias(self, conn):
        from anofox import validate
        with pytest.warns(DeprecationWarning, match="format="):
            assert validate.phone_format(conn, "+49 30 901820", fmt="RFC3966") == "tel:+49-30-901820"

    def test_both_raise(self, conn):
        from anofox import validate
        with pytest.raises(TypeError, match="fmt"):
            validate.phone_format(conn, "+49 30 901820", format="E164", fmt="E164")


class TestPhoneNumbersMustBeText:
    """A phone number read as a number has lost its leading 0: reject it clearly, never convert."""

    @pytest.mark.parametrize("func", ["phone_is_valid", "phone_parse", "phone_format", "phone_region"])
    def test_int_scalar_raises_type_error(self, conn, func):
        import numpy as np
        from anofox import validate
        for value in (4930901820, np.int64(4930901820), 30901820.0):
            with pytest.raises(TypeError, match=r"phone numbers must be text.*astype\(str\)"):
                getattr(validate, func)(conn, value)

    @pytest.mark.parametrize("func", ["phone_is_valid", "phone_parse", "phone_format", "phone_region"])
    def test_int64_column_raises_type_error(self, conn, func):
        import pandas as pd
        from anofox import validate
        # read_csv turns "030 901820"-style numbers without separators into int64
        crm = pd.DataFrame({"customer": ["C-17", "C-18"], "phone": [30901820, 4930901820]})
        with pytest.raises(TypeError, match=r"column 'phone'.*phone numbers must be text.*astype\(str\)"):
            getattr(validate, func)(conn, crm, column="phone")

    def test_polars_int_column_raises_type_error(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import validate
        crm = pl.DataFrame({"phone": [30901820, 4930901820]})
        with pytest.raises(TypeError, match="phone numbers must be text"):
            validate.phone_is_valid(conn, crm, column="phone", region="DE")

    def test_text_column_converted_by_caller_works(self, conn):
        import pandas as pd
        from anofox import validate
        crm = pd.DataFrame({"phone": [4930901820]})
        out = validate.phone_format(conn, "+" + crm.astype({"phone": str}), column="phone")
        assert out["phone_format"].tolist() == ["+4930901820"]

    def test_all_missing_column_still_accepted(self, conn):
        import pandas as pd
        from anofox import validate
        crm = pd.DataFrame({"phone": [float("nan"), float("nan")]})
        out = validate.phone_is_valid(conn, crm, column="phone")
        assert out["phone_is_valid"].isna().all()
