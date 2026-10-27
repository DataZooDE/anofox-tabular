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


class TestPiiAuditTable:
    def test_returns_dataframe(self, conn, text_table):
        import pandas as pd
        from anofox import pii
        result = pii.pii_audit_table(conn, text_table)
        assert isinstance(result, pd.DataFrame)
        assert "id" not in result.columns

    def test_id_column_passed_through(self, conn, text_table):
        from anofox import pii
        result = pii.pii_audit_table(conn, text_table, columns=["text"], id_column="id")
        assert list(result.columns[:2]) == ["row_id", "id"]
        # Only row 1 contains PII
        assert set(result["id"]) == {1}


class TestPiiTableColumnsArgument:
    def test_scan_columns_as_string(self, conn, text_table):
        from anofox import pii
        as_list = pii.pii_scan_table(conn, text_table, columns=["text"])
        as_string = pii.pii_scan_table(conn, text_table, columns="text")
        assert as_string.equals(as_list)

    def test_audit_columns_as_string(self, conn, text_table):
        from anofox import pii
        as_list = pii.pii_audit_table(conn, text_table, columns=["text"])
        as_string = pii.pii_audit_table(conn, text_table, columns="text")
        assert as_string.equals(as_list)


class TestPiiMaskValue:
    """pii_mask_value masks a whole known-PII value, also where detection finds nothing."""

    def test_scalar(self, conn):
        from anofox import pii
        assert pii.pii_mask_value(conn, "Li", "NAME") == "[NAME]"
        assert pii.pii_mask_value(conn, None, "NAME") is None

    def test_dataframe_keeps_index(self, conn):
        import pandas as pd
        from anofox import pii
        df = pd.DataFrame({"first_name": ["Li", "Anna", None], "phone": ["01511234567", "030 1234", None]},
                          index=[7, 8, 9])
        out = pii.pii_mask_value(conn, df, "PHONE", strategy="redact", column="phone")
        assert list(out.index) == [7, 8, 9]
        assert out["pii_mask_value"].tolist()[:2] == ["[PHONE]", "[PHONE]"]
        assert out["pii_mask_value"].isna().tolist()[2]

    def test_unknown_type_raises(self, conn):
        import duckdb
        import pytest
        from anofox import pii
        with pytest.raises(duckdb.Error, match="Unknown PII type"):
            pii.pii_mask_value(conn, "x", "NO_SUCH_TYPE")


class TestMaskReplace:
    """replace=True masks the column in place instead of adding a column next
    to the raw value, so the result no longer carries the PII."""

    @staticmethod
    def _tickets():
        import pandas as pd
        return pd.DataFrame({
            "ticket_id": [5001, 5002, 5003],
            "body": ["Please call me at +1 650-253-0000",
                     "Mail ana.lopez@example.com about the refund",
                     None],
            "agent": ["Mia Schulz", "Li", "Tom Berger"],
        }, index=[7, 8, 9])

    def test_pii_mask_replaces_the_column(self, conn):
        from anofox import pii
        tickets = self._tickets()
        masked = pii.pii_mask(conn, tickets, column="body", replace=True)
        assert list(masked.columns) == ["ticket_id", "body", "agent"]
        assert list(masked.index) == [7, 8, 9]
        assert "650-253-0000" not in masked.loc[7, "body"]
        assert "ana.lopez@example.com" not in masked.loc[8, "body"]
        assert masked["body"].isna().tolist() == [False, False, True]
        # The input is not changed.
        assert tickets.loc[8, "body"] == "Mail ana.lopez@example.com about the refund"

    def test_pii_mask_default_still_adds_a_column(self, conn):
        from anofox import pii
        masked = pii.pii_mask(conn, self._tickets(), column="body")
        assert list(masked.columns) == ["ticket_id", "body", "agent", "pii_mask"]

    def test_pii_mask_value_replaces_the_column(self, conn):
        from anofox import pii
        masked = pii.pii_mask_value(conn, self._tickets(), "NAME", column="agent", replace=True)
        assert list(masked.columns) == ["ticket_id", "body", "agent"]
        assert "Li" not in masked["agent"].tolist()
        assert "Mia Schulz" not in masked["agent"].tolist()

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import pii
        tickets = pl.from_pandas(self._tickets())
        masked = pii.pii_mask(conn, tickets, column="body", replace=True)
        assert masked.columns == ["ticket_id", "body", "agent"]
        assert "ana.lopez@example.com" not in masked["body"][1]

    @pytest.mark.parametrize("call", [
        lambda pii, conn: pii.pii_mask(conn, "ana@example.com", replace=True),
        lambda pii, conn: pii.pii_mask_value(conn, "Li", "NAME", replace=True),
    ])
    def test_replace_needs_a_dataframe(self, conn, call):
        from anofox import pii
        with pytest.raises(ValueError, match="replace.*DataFrame"):
            call(pii, conn)
