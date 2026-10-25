"""
Tests for anomaly.py — assert Python API contract (return types, not cluster quality).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def numeric_table(conn):
    """Create a table with numeric data for anomaly tests."""
    conn.execute(
        "CREATE OR REPLACE TABLE _anomaly AS "
        "SELECT unnest(range(1, 51)) AS id, "
        "       unnest(range(1, 51))::DOUBLE AS x, "
        "       (unnest(range(1, 51)) * 2)::DOUBLE AS y"
    )
    yield "_anomaly"
    conn.execute("DROP TABLE IF EXISTS _anomaly")


class TestIsolationForest:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores")
        assert isinstance(result, pd.DataFrame)
        assert len(result) > 0

    def test_dataframe_input_summary(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 31))})
        result = anomaly.isolation_forest(conn, df, "val", output_mode="summary")
        assert isinstance(result, dict)

    def test_dataframe_input_scores(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 31))})
        result = anomaly.isolation_forest(conn, df, "val", output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_raises_for_non_df_non_str(self, conn):
        from anofox import anomaly
        with pytest.raises(TypeError, match="pandas or polars"):
            anomaly.isolation_forest(conn, [1, 2, 3], "col")


class TestIsolationForestMv:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"a": list(range(1, 21)), "b": list(range(1, 21))})
        result = anomaly.isolation_forest_mv(conn, df, ["a", "b"], output_mode="summary")
        assert isinstance(result, dict)


class TestDbscan:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.dbscan(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.dbscan(conn, numeric_table, "x", output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 21))})
        result = anomaly.dbscan(conn, df, "val", output_mode="summary")
        assert isinstance(result, dict)


class TestDbscanMv:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, numeric_table, ["x", "y"], output_mode="scores")
        assert isinstance(result, pd.DataFrame)


class TestOutlierTree:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_outliers_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, ["x", "y"], output_mode="outliers")
        assert isinstance(result, pd.DataFrame)

    def test_single_column_string(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"a": list(range(1, 31)), "b": list(range(1, 31))})
        result = anomaly.outlier_tree(conn, df, ["a", "b"], output_mode="summary")
        assert isinstance(result, dict)


@pytest.fixture(scope="module")
def id_table(conn):
    """Numeric data with a planted outlier and an id that is not the position."""
    conn.execute(
        "CREATE OR REPLACE TABLE _anomaly_ids AS "
        "SELECT 'K' || (1000 - i) AS key, "
        "       CASE WHEN i = 37 THEN 10000.0 ELSE (i % 5)::DOUBLE END AS x, "
        "       CASE WHEN i = 37 THEN 10000.0 ELSE ((i % 5) * 2)::DOUBLE END AS y "
        "FROM range(1, 61) t(i)"
    )
    yield "_anomaly_ids"
    conn.execute("DROP TABLE IF EXISTS _anomaly_ids")


class TestIdColumn:
    def _assert_id_after_row_id(self, result):
        assert list(result.columns[:2]) == ["row_id", "key"]

    def test_isolation_forest(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, id_table, "x", output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert result.sort_values("anomaly_score", ascending=False)["key"].iloc[0] == "K963"

    def test_isolation_forest_mv(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, id_table, ["x", "y"], output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert result.sort_values("anomaly_score", ascending=False)["key"].iloc[0] == "K963"

    def test_dbscan(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.dbscan(conn, id_table, "x", output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert list(result[result["is_anomaly"]]["key"]) == ["K963"]

    def test_dbscan_mv(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, id_table, ["x", "y"], output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert list(result[result["is_anomaly"]]["key"]) == ["K963"]

    def test_outlier_tree(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, id_table, ["x", "y"], output_mode="outliers", id_column="key")
        self._assert_id_after_row_id(result)

    def test_rejected_in_summary_mode(self, conn, id_table):
        import duckdb
        from anofox import anomaly
        with pytest.raises(duckdb.Error, match="id_column requires output_mode"):
            anomaly.isolation_forest(conn, id_table, "x", output_mode="summary", id_column="key")


class TestOutputModes:
    """Every per-row mode returns a DataFrame; unknown modes raise ValueError."""

    @pytest.mark.parametrize("func,arg", [("dbscan", "x"), ("dbscan_mv", ["x", "y"])])
    def test_dbscan_clusters_mode_returns_dataframe(self, conn, numeric_table, func, arg):
        import pandas as pd
        from anofox import anomaly
        result = getattr(anomaly, func)(conn, numeric_table, arg, output_mode="clusters")
        assert isinstance(result, pd.DataFrame)
        assert len(result) == 50
        assert "cluster_id" in result.columns

    @pytest.mark.parametrize("func,arg,mode", [
        ("isolation_forest", "x", "clusters"),
        ("isolation_forest", "x", "outliers"),
        ("isolation_forest_mv", ["x", "y"], "Scores"),
        ("dbscan", "x", "outliers"),
        ("dbscan_mv", ["x", "y"], "summery"),
        ("outlier_tree", ["x", "y"], "scores"),
    ])
    def test_unknown_mode_raises(self, conn, numeric_table, func, arg, mode):
        from anofox import anomaly
        with pytest.raises(ValueError, match="output_mode"):
            getattr(anomaly, func)(conn, numeric_table, arg, output_mode=mode)


@pytest.fixture(scope="module")
def staff_table(conn):
    conn.execute(
        "CREATE OR REPLACE TABLE _staff AS "
        "SELECT i AS staff_id, CASE WHEN i <= 100 THEN 'junior' ELSE 'senior' END AS level, "
        "CASE WHEN i = 7 THEN 92000 WHEN i <= 100 THEN 48000 + (i % 10) * 500 "
        "ELSE 88000 + (i % 10) * 600 END AS salary FROM range(1, 201) t(i)"
    )
    yield "_staff"
    conn.execute("DROP TABLE IF EXISTS _staff")


class TestOutlierTreeDefaults:
    """Python defaults are the SQL defaults: the same call gives the same result."""

    def test_summary_matches_sql_defaults(self, conn, staff_table):
        from anofox import anomaly
        cursor = conn.execute(f"SELECT * FROM outlier_tree('{staff_table}', 'level, salary', 'summary')")
        names = [d[0] for d in cursor.description]
        expected = dict(zip(names, cursor.fetchone()))
        assert anomaly.outlier_tree(conn, staff_table, ["level", "salary"]) == expected

    def test_outliers_match_sql_defaults(self, conn, staff_table):
        from anofox import anomaly
        expected = conn.execute(
            f"SELECT row_id FROM outlier_tree('{staff_table}', 'level, salary', 'outliers') ORDER BY row_id"
        ).fetchall()
        out = anomaly.outlier_tree(conn, staff_table, ["level", "salary"], output_mode="outliers")
        assert sorted(out["row_id"].tolist()) == [r[0] for r in expected]

    def test_explicit_parameter_overrides_default(self, conn, staff_table):
        from anofox import anomaly
        loose = anomaly.outlier_tree(conn, staff_table, ["level", "salary"], z_outlier=1.0,
                                     min_size_numeric=10, max_perc_outliers=0.5)
        strict = anomaly.outlier_tree(conn, staff_table, ["level", "salary"])
        assert loose["outlier_count"] >= strict["outlier_count"]


class TestIsolationForestSeed:
    """seed= makes scores reproducible, as the SQL seed parameter does."""

    def test_same_seed_same_scores(self, conn, numeric_table):
        from anofox import anomaly
        a = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=42)
        b = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=42)
        assert a["anomaly_score"].tolist() == b["anomaly_score"].tolist()

    def test_seed_matches_sql(self, conn, numeric_table):
        from anofox import anomaly
        sql = conn.execute(
            f"SELECT anomaly_score FROM isolation_forest('{numeric_table}', 'x', 100, 256, 0.1, 'scores', 7) "
            "ORDER BY row_id"
        ).fetchall()
        py = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=7)
        assert py["anomaly_score"].tolist() == [r[0] for r in sql]

    def test_mv_same_seed_same_scores(self, conn, numeric_table):
        from anofox import anomaly
        kwargs = dict(output_mode="scores", seed=3, ndim=2)
        a = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], **kwargs)
        b = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], **kwargs)
        assert a["anomaly_score"].tolist() == b["anomaly_score"].tolist()

    def test_mv_ndim_matches_sql(self, conn, numeric_table):
        from anofox import anomaly
        sql = conn.execute(
            f"SELECT anomaly_score FROM isolation_forest_mv('{numeric_table}', 'x, y', 100, 256, 0.1, "
            "'scores', 5, 2) ORDER BY row_id"
        ).fetchall()
        py = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="scores", seed=5, ndim=2)
        assert py["anomaly_score"].tolist() == [r[0] for r in sql]
