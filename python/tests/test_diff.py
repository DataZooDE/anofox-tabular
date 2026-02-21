"""
Tests for diff.py — assert Python API contract (return types and change detection).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def diff_tables(conn):
    """Create source and target tables with a known difference."""
    conn.execute(
        "CREATE OR REPLACE TABLE _diff_src AS "
        "SELECT 1 AS id, 'Alice' AS name, 100.0 AS amount "
        "UNION ALL SELECT 2, 'Bob', 200.0 "
        "UNION ALL SELECT 3, 'Carol', 300.0"
    )
    conn.execute(
        "CREATE OR REPLACE TABLE _diff_tgt AS "
        "SELECT 1 AS id, 'Alice' AS name, 150.0 AS amount "  # changed amount
        "UNION ALL SELECT 2, 'Bob', 200.0 "                  # unchanged
        "UNION ALL SELECT 4, 'Dave', 400.0"                  # added row (3 removed)
    )
    yield "_diff_src", "_diff_tgt"
    conn.execute("DROP TABLE IF EXISTS _diff_src")
    conn.execute("DROP TABLE IF EXISTS _diff_tgt")


class TestJoindiff:
    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import diff
        src_df = pd.DataFrame({"id": [1, 2, 3], "val": [10.0, 20.0, 30.0]})
        tgt_df = pd.DataFrame({"id": [1, 2, 4], "val": [10.0, 99.0, 40.0]})
        result = diff.joindiff(conn, src_df, tgt_df, primary_keys="id")
        assert isinstance(result, pd.DataFrame)
        assert len(result) > 0

    def test_returns_dataframe(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result = diff.joindiff(conn, src, tgt, primary_keys="id")
        assert isinstance(result, pd.DataFrame)

    def test_detects_changed_row(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result = diff.joindiff(conn, src, tgt, primary_keys="id")
        # Should contain at least one row (changed, added, or removed)
        assert len(result) > 0

    def test_result_has_diff_type_column(self, conn, diff_tables):
        from anofox import diff
        src, tgt = diff_tables
        result = diff.joindiff(conn, src, tgt, primary_keys="id")
        assert "diff_type" in result.columns

    def test_with_compare_columns(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result = diff.joindiff(conn, src, tgt, primary_keys="id", compare_columns=["amount"])
        assert isinstance(result, pd.DataFrame)

    def test_include_unchanged(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result_excl = diff.joindiff(conn, src, tgt, primary_keys="id", include_unchanged=False)
        result_incl = diff.joindiff(conn, src, tgt, primary_keys="id", include_unchanged=True)
        # Including unchanged should have >= rows than excluding
        assert len(result_incl) >= len(result_excl)


class TestHashdiff:
    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import diff
        src_df = pd.DataFrame({"id": [1, 2, 3], "val": [10.0, 20.0, 30.0]})
        tgt_df = pd.DataFrame({"id": [1, 2, 4], "val": [10.0, 99.0, 40.0]})
        result = diff.hashdiff(conn, src_df, tgt_df, primary_keys="id")
        assert isinstance(result, pd.DataFrame)

    def test_returns_dataframe(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result = diff.hashdiff(conn, src, tgt, primary_keys="id")
        assert isinstance(result, pd.DataFrame)

    def test_detects_differences(self, conn, diff_tables):
        from anofox import diff
        src, tgt = diff_tables
        result = diff.hashdiff(conn, src, tgt, primary_keys="id")
        assert len(result) > 0

    def test_with_bisection_threshold(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        # bisection_threshold is optional — exercise the parameter path
        result = diff.hashdiff(conn, src, tgt, primary_keys="id", bisection_threshold=1000)
        assert isinstance(result, pd.DataFrame)
