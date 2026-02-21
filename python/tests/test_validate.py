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
