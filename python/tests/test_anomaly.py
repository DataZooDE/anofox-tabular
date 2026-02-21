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
