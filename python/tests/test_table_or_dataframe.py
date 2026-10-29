"""
Functions documented as taking a table name also take a pandas or polars
DataFrame; anything else raises TypeError instead of being pasted into SQL.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")


def _anofox_tables(conn):
    return conn.execute(
        "SELECT count(*) FROM information_schema.tables WHERE table_name LIKE '_anofox_%'"
    ).fetchone()[0]


@pytest.fixture
def frame():
    return pd.DataFrame({"id": [1, 2, 3, 4], "email": ["a@b.com", None, "c@d.org", "e@f.net"],
                         "amount": [1.0, 2.0, 3.0, 4.0]})


class TestQualityAcceptsDataFrames:
    @pytest.mark.parametrize("to_frame", [lambda d: d, pl.from_pandas], ids=["pandas", "polars"])
    def test_checks(self, conn, frame, to_frame):
        from anofox import quality
        df = to_frame(frame)
        before = _anofox_tables(conn)
        assert quality.volume(conn, df, min_rows=4)["status"] == "pass"
        assert quality.null_rate(conn, df, "email", max_null_rate=0.1)["status"] == "fail"
        assert quality.distinct_count(conn, df, "id", min_distinct=4)["status"] == "pass"
        assert quality.schema_check(conn, df, ["id", "email"])["status"] == "pass"
        assert quality.zscore(conn, df, "amount")["status"] == "pass"
        assert quality.duplicate_count(conn, df, "id")["status"] == "pass"
        assert quality.compliance(conn, df, "amount > 0")["status"] == "pass"
        assert _anofox_tables(conn) == before

    def test_match_rate_with_two_frames(self, conn, frame):
        from anofox import quality
        right = pd.DataFrame({"id": [1, 2]})
        result = quality.match_rate(conn, frame, right, "id", "id", min_rate=0.5)
        assert result["status"] == "pass"

    def test_other_objects_raise_type_error(self, conn):
        from anofox import quality
        with pytest.raises(TypeError, match="pass a table name, a DataFrame"):
            quality.volume(conn, 42, min_rows=1)
        with pytest.raises(TypeError, match="pass a table name, a DataFrame"):
            quality.null_rate(conn, None, "x")


class TestPiiTablesAcceptDataFrames:
    def test_scan_polars(self, conn):
        from anofox import pii
        out = pii.pii_scan_table(conn, pl.DataFrame({"t": ["mail a@b.com"]}))
        assert isinstance(out, pl.DataFrame)
        assert set(out["pii_type"].to_list()) == {"EMAIL"}

    def test_audit_pandas(self, conn):
        from anofox import pii
        out = pii.pii_audit_table(conn, pd.DataFrame({"k": [9], "t": ["mail a@b.com"]}), id_column="k")
        assert out["k"].tolist() == [9]

    def test_other_objects_raise_type_error(self, conn):
        from anofox import pii
        with pytest.raises(TypeError, match="pass a table name, a DataFrame"):
            pii.pii_scan_table(conn, ["a"])
