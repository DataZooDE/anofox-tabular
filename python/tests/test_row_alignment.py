"""
Row-wise wrappers return the input's rows in the input's order, with the
pandas index kept, so a result column can be assigned back to the input.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")


@pytest.fixture
def indexed_emails():
    return pd.DataFrame({"email": ["a@b.com", "bad", "c@d.org", "x y@z"]}, index=[10, 20, 30, 40])


class TestPandasIndex:
    def test_assign_back_to_custom_index(self, conn, indexed_emails):
        from anofox import validate
        big = indexed_emails.copy()
        big["ok"] = validate.email_is_valid(conn, big, column="email")["email_is_valid"]
        assert big["ok"].tolist() == [True, False, True, False]

    def test_result_keeps_index(self, conn, indexed_emails):
        from anofox import validate
        out = validate.email_is_valid(conn, indexed_emails, column="email")
        assert out.index.tolist() == [10, 20, 30, 40]

    def test_original_columns_untouched(self, conn):
        from anofox import validate
        df = pd.DataFrame({"email": ["a@b.com", None], "n": pd.array([1, None], dtype="Int64")})
        out = validate.email_is_valid(conn, df, column="email")
        assert out["email"].equals(df["email"])
        assert str(out["n"].dtype) == "Int64"
        assert out["email_is_valid"].iloc[0] is True or out["email_is_valid"].iloc[0] == True  # noqa: E712

    def test_duplicate_index_labels(self, conn):
        from anofox import validate
        df = pd.DataFrame({"email": ["a@b.com", "bad", "c@d.org"]}, index=["k", "k", "j"])
        out = validate.email_is_valid(conn, df, column="email")
        assert out.index.tolist() == ["k", "k", "j"]
        assert out["email_is_valid"].tolist() == [True, False, True]

    @pytest.mark.parametrize("func,column,result", [
        ("email_validate", "email", "email_validate"),
        ("phone_is_valid", "phone", "phone_is_valid"),
        ("phone_parse", "phone", "phone_parse"),
    ])
    def test_other_validate_wrappers_keep_index(self, conn, func, column, result):
        from anofox import validate
        df = pd.DataFrame({"email": ["a@b.com", "bad"], "phone": ["+14155552671", "x"]}, index=[5, 3])
        out = getattr(validate, func)(conn, df, column=column)
        assert out.index.tolist() == [5, 3]
        assert result in out.columns

    def test_pii_detect_keeps_index(self, conn):
        from anofox import pii
        df = pd.DataFrame({"t": ["mail a@b.com", "nothing"]}, index=[7, 2])
        out = pii.pii_detect(conn, df, column="t")
        assert out.index.tolist() == [7, 2]
        assert len(out["pii_detect"].iloc[0]) >= 1
        assert len(out["pii_detect"].iloc[1]) == 0


class TestPolarsOrder:
    def test_large_frame_keeps_row_order(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import validate
        n = 20_000
        emails = [f"user{i}@example.com" if i % 3 else f"bad{i}" for i in range(n)]
        df = pl.DataFrame({"i": list(range(n)), "email": emails})
        out = validate.email_is_valid(conn, df, column="email")
        assert out["i"].to_list() == list(range(n))
        assert out["email_is_valid"].to_list() == [bool(i % 3) for i in range(n)]


class TestValidateRowId:
    def test_row_id_is_index_label(self, conn, indexed_emails):
        from anofox.validate import EmailRule
        result = conn.validate(indexed_emails, {"email": EmailRule()})
        assert sorted(result.failures["row_id"].tolist()) == [20, 40]

    def test_row_id_is_position_for_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox.validate import EmailRule
        df = pl.DataFrame({"email": ["a@b.com", "bad", "c@d.org", "x y@z"]})
        result = conn.validate(df, {"email": EmailRule()})
        assert sorted(result.failures["row_id"].to_list()) == [1, 3]


class TestPositionColumnCollision:
    """A DataFrame column named like the internal row-position column is used as data."""

    def test_row_wise_on_column_named_anofox_pos(self, conn):
        import pandas as pd
        from anofox import validate
        df = pd.DataFrame({"__anofox_pos": ["ana@example.com", "bob(at)example.com"]}, index=[7, 3])
        out = validate.email_is_valid(conn, df, column="__anofox_pos")
        assert out["email_is_valid"].tolist() == [True, False]
        assert out.index.tolist() == [7, 3]

    def test_validate_on_column_named_anofox_pos(self, conn):
        import pandas as pd
        from anofox.validate import EmailRule
        df = pd.DataFrame({"__anofox_pos": ["ana@example.com", "bob(at)example.com"]}, index=[7, 3])
        result = conn.validate(df, {"__anofox_pos": EmailRule(mode="regex")})
        assert result.failures["row_id"].tolist() == [3]
        assert result.failures["value"].tolist() == ["bob(at)example.com"]
