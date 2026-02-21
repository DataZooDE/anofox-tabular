"""
Tests for quality.py — assert Python API contract shape and return types.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


@pytest.fixture(scope="module")
def quality_table(conn):
    """Create a temp table for quality tests."""
    conn.execute(
        "CREATE OR REPLACE TABLE _qtest AS "
        "SELECT unnest(range(1, 101)) AS id, "
        "       unnest(range(1, 101))::DOUBLE AS score, "
        "       'cat' || (unnest(range(1, 101)) % 5)::VARCHAR AS category, "
        "       now() - INTERVAL (unnest(range(1, 101))) MINUTE AS ts"
    )
    yield "_qtest"
    conn.execute("DROP TABLE IF EXISTS _qtest")


@pytest.fixture(scope="module")
def sparse_table(conn):
    """Table with some nulls."""
    conn.execute(
        "CREATE OR REPLACE TABLE _sparse AS "
        "SELECT id, CASE WHEN id % 4 = 0 THEN NULL ELSE id::DOUBLE END AS val "
        "FROM (SELECT unnest(range(1, 21)) AS id)"
    )
    yield "_sparse"
    conn.execute("DROP TABLE IF EXISTS _sparse")


class TestVolume:
    def test_returns_dict(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=1)
        assert isinstance(result, dict)

    def test_has_status_key(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=1)
        assert "status" in result

    def test_status_is_pass_or_fail(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=1)
        assert result["status"] in ("pass", "fail", "warn", "error")

    def test_pass_when_within_range(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=50, max_rows=200)
        assert result["status"] == "pass"

    def test_fail_when_below_min(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=1000)
        assert result["status"] == "fail"

    def test_raises_for_negative_min_rows(self, conn, quality_table):
        from anofox import quality
        with pytest.raises(ValueError, match="min_rows must be >= 0"):
            quality.volume(conn, quality_table, min_rows=-1)

    def test_raises_when_max_below_min(self, conn, quality_table):
        from anofox import quality
        with pytest.raises(ValueError, match="max_rows must be >= min_rows"):
            quality.volume(conn, quality_table, min_rows=10, max_rows=5)


class TestNullRate:
    def test_returns_dict(self, conn, sparse_table):
        from anofox import quality
        result = quality.null_rate(conn, sparse_table, "val", max_null_rate=1.0)
        assert isinstance(result, dict)

    def test_has_status(self, conn, sparse_table):
        from anofox import quality
        result = quality.null_rate(conn, sparse_table, "val", max_null_rate=1.0)
        assert "status" in result

    def test_pass_when_below_threshold(self, conn, sparse_table):
        from anofox import quality
        # 25% nulls, threshold 50%
        result = quality.null_rate(conn, sparse_table, "val", max_null_rate=0.5)
        assert result["status"] == "pass"

    def test_fail_when_above_threshold(self, conn, sparse_table):
        from anofox import quality
        # 25% nulls but threshold is 0%
        result = quality.null_rate(conn, sparse_table, "val", max_null_rate=0.0)
        assert result["status"] == "fail"


class TestDistinctCount:
    def test_returns_dict(self, conn, quality_table):
        from anofox import quality
        result = quality.distinct_count(conn, quality_table, "category", min_distinct=1)
        assert isinstance(result, dict)

    def test_has_status(self, conn, quality_table):
        from anofox import quality
        result = quality.distinct_count(conn, quality_table, "category", min_distinct=1)
        assert "status" in result

    def test_pass_when_enough_distinct(self, conn, quality_table):
        from anofox import quality
        # category has 5 distinct values (cat0..cat4)
        result = quality.distinct_count(conn, quality_table, "category", min_distinct=3, max_distinct=10)
        assert result["status"] == "pass"

    def test_fail_when_too_few_distinct(self, conn, quality_table):
        from anofox import quality
        result = quality.distinct_count(conn, quality_table, "category", min_distinct=100)
        assert result["status"] == "fail"


class TestZscore:
    def test_returns_dict(self, conn, quality_table):
        from anofox import quality
        result = quality.zscore(conn, quality_table, "score")
        assert isinstance(result, dict)

    def test_has_status(self, conn, quality_table):
        from anofox import quality
        result = quality.zscore(conn, quality_table, "score")
        assert "status" in result


class TestIqr:
    def test_returns_dict(self, conn, quality_table):
        from anofox import quality
        result = quality.iqr(conn, quality_table, "score")
        assert isinstance(result, dict)

    def test_has_status(self, conn, quality_table):
        from anofox import quality
        result = quality.iqr(conn, quality_table, "score")
        assert "status" in result


class TestSchemaCheck:
    def test_returns_dict(self, conn, quality_table):
        from anofox import quality
        result = quality.schema_check(conn, quality_table, ["id", "score"])
        assert isinstance(result, dict)

    def test_has_status(self, conn, quality_table):
        from anofox import quality
        result = quality.schema_check(conn, quality_table, ["id"])
        assert "status" in result

    def test_pass_when_columns_present(self, conn, quality_table):
        from anofox import quality
        result = quality.schema_check(conn, quality_table, ["id", "score"])
        assert result["status"] == "pass"

    def test_fail_when_column_missing(self, conn, quality_table):
        from anofox import quality
        result = quality.schema_check(conn, quality_table, ["id", "nonexistent_col"])
        assert result["status"] == "fail"
