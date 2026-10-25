"""
A polars input gives a polars result; a pandas input or a table name gives pandas.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")


@pytest.fixture
def readings():
    values = [10.0 + (i % 5) for i in range(39)] + [95.0]
    return pl.DataFrame({"id": list(range(1, 41)), "value": values, "level": ["a", "b"] * 20})


class TestPolarsInPolarsOut:
    def test_anomaly_row_outputs(self, conn, readings):
        from anofox import anomaly
        assert isinstance(anomaly.isolation_forest(conn, readings, "value", output_mode="scores"), pl.DataFrame)
        assert isinstance(anomaly.isolation_forest_mv(conn, readings, ["value", "id"], output_mode="scores"), pl.DataFrame)
        assert isinstance(anomaly.dbscan(conn, readings, "value", output_mode="scores"), pl.DataFrame)
        assert isinstance(anomaly.dbscan_mv(conn, readings, ["value", "id"], output_mode="scores"), pl.DataFrame)
        assert isinstance(anomaly.outlier_tree(conn, readings, ["level", "value"], output_mode="outliers"), pl.DataFrame)

    def test_profile(self, conn, readings):
        from anofox import profile
        assert isinstance(profile.profile_table(conn, readings), pl.DataFrame)
        assert isinstance(profile.profile_summary(conn, readings), pl.DataFrame)
        assert isinstance(profile.profile_correlations(conn, readings), pl.DataFrame)

    def test_diff(self, conn, readings):
        from anofox import diff
        changed = readings.with_columns(pl.col("value") + 1)
        assert isinstance(diff.joindiff(conn, readings, changed, primary_keys="id"), pl.DataFrame)
        assert isinstance(diff.hashdiff(conn, readings, changed, primary_keys="id"), pl.DataFrame)

    def test_connection_profile(self, conn, readings):
        out = conn.profile(readings)
        assert isinstance(out, pl.DataFrame)
        assert out["column"].to_list() == ["id", "value", "level"]

    def test_connection_validate_failures(self, conn):
        from anofox.validate import EmailRule
        result = conn.validate(pl.DataFrame({"email": ["a@b.com", "bad"]}), {"email": EmailRule()})
        assert isinstance(result.failures, pl.DataFrame)
        assert result.failures["row_id"].to_list() == [1]
        assert "FAILED" in result._repr_html_()
        ok = conn.validate(pl.DataFrame({"email": ["a@b.com"]}), {"email": EmailRule()})
        assert isinstance(ok.failures, pl.DataFrame) and len(ok.failures) == 0
        assert "No failures" in ok._repr_html_()


class TestPandasStaysPandas:
    def test_table_name_gives_pandas(self, conn, readings):
        from anofox import profile
        conn.execute("CREATE OR REPLACE TABLE _dflib AS SELECT * FROM range(3) t(i)")
        assert isinstance(profile.profile_summary(conn, "_dflib"), pd.DataFrame)
        assert isinstance(conn.profile("_dflib"), pd.DataFrame)

    def test_pandas_gives_pandas(self, conn, readings):
        from anofox import anomaly
        out = anomaly.dbscan(conn, readings.to_pandas(), "value", output_mode="scores")
        assert isinstance(out, pd.DataFrame)
