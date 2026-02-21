"""
Tests for pii.py — assert Python API contract (return types, not detection accuracy).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def text_table(conn):
    """Create a table with text data for PII tests."""
    conn.execute(
        "CREATE OR REPLACE TABLE _pii_test AS "
        "SELECT 1 AS id, 'Call me at +14155552671 or email test@example.com' AS text "
        "UNION ALL "
        "SELECT 2, 'No PII here, just regular text about nothing sensitive'"
    )
    yield "_pii_test"
    conn.execute("DROP TABLE IF EXISTS _pii_test")


class TestPiiDetect:
    def test_returns_list_for_string(self, conn):
        from anofox import pii
        result = pii.pii_detect(conn, "test@example.com is my email")
        assert isinstance(result, list)

    def test_each_item_is_dict(self, conn):
        from anofox import pii
        result = pii.pii_detect(conn, "test@example.com is my email")
        for item in result:
            assert isinstance(item, dict)

    def test_empty_string_returns_list(self, conn):
        from anofox import pii
        result = pii.pii_detect(conn, "no pii here")
        assert isinstance(result, list)

    def test_dataframe_returns_dataframe(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        df = conn.execute(f"SELECT * FROM {text_table}").df()
        result = pii.pii_detect(conn, df, column="text")
        assert isinstance(result, pd.DataFrame)
        assert "pii_detect" in result.columns

    def test_dataframe_preserves_row_count(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        df = conn.execute(f"SELECT * FROM {text_table}").df()
        result = pii.pii_detect(conn, df, column="text")
        assert len(result) == 2

    def test_raises_when_df_without_column(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        df = conn.execute(f"SELECT * FROM {text_table}").df()
        with pytest.raises(ValueError, match="'column' is required"):
            pii.pii_detect(conn, df)


class TestPiiMask:
    def test_returns_string(self, conn):
        from anofox import pii
        result = pii.pii_mask(conn, "test@example.com is my email")
        assert isinstance(result, str)

    def test_masked_string_is_non_empty(self, conn):
        from anofox import pii
        result = pii.pii_mask(conn, "hello world")
        assert isinstance(result, str)

    def test_strategy_redact(self, conn):
        from anofox import pii
        result = pii.pii_mask(conn, "Call +14155552671", strategy="redact")
        assert isinstance(result, str)

    def test_strategy_asterisk(self, conn):
        from anofox import pii
        result = pii.pii_mask(conn, "test@example.com", strategy="asterisk")
        assert isinstance(result, str)


class TestPiiContains:
    def test_returns_bool(self, conn):
        from anofox import pii
        result = pii.pii_contains(conn, "test@example.com")
        assert isinstance(result, bool)

    def test_text_with_email_returns_true(self, conn):
        from anofox import pii
        result = pii.pii_contains(conn, "my email is test@example.com")
        assert result is True

    def test_plain_text_returns_false(self, conn):
        from anofox import pii
        result = pii.pii_contains(conn, "hello world this is plain text")
        assert result is False


class TestPiiScanTable:
    def test_returns_dataframe(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        result = pii.pii_scan_table(conn, text_table)
        assert isinstance(result, pd.DataFrame)

    def test_dataframe_has_columns(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        result = pii.pii_scan_table(conn, text_table)
        # Should have some meaningful columns (column_name, pii_type, etc.)
        assert len(result.columns) > 0
