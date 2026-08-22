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


class TestRegexMatch:
    def test_pass_when_all_match(self, conn, quality_table):
        from anofox import quality
        result = quality.regex_match(conn, quality_table, "category", "^cat[0-9]$")
        assert result["status"] == "pass"

    def test_fail_below_min_rate(self, conn, quality_table):
        from anofox import quality
        result = quality.regex_match(conn, quality_table, "category", "^cat0$", min_match_rate=0.9)
        assert result["status"] == "fail"

    def test_max_rate_bound(self, conn, quality_table):
        from anofox import quality
        result = quality.regex_match(
            conn, quality_table, "category", "^cat0$", min_match_rate=0.0, max_match_rate=0.1
        )
        assert result["status"] == "fail"


class TestValuesInSet:
    def test_pass_when_all_in_set(self, conn, quality_table):
        from anofox import quality
        allowed = [f"cat{i}" for i in range(5)]
        result = quality.values_in_set(conn, quality_table, "category", allowed)
        assert result["status"] == "pass"

    def test_fail_when_value_missing_from_set(self, conn, quality_table):
        from anofox import quality
        result = quality.values_in_set(conn, quality_table, "category", ["cat0", "cat1"])
        assert result["status"] == "fail"

    def test_min_rate_relaxes_check(self, conn, quality_table):
        from anofox import quality
        result = quality.values_in_set(
            conn, quality_table, "category", ["cat0", "cat1"], min_rate=0.3
        )
        assert result["status"] == "pass"


class TestAggCheck:
    def test_avg_within_bounds(self, conn, quality_table):
        from anofox import quality
        result = quality.agg_check(conn, quality_table, "score", "avg", 1.0, 100.0)
        assert result["status"] == "pass"

    def test_max_exceeds_upper(self, conn, quality_table):
        from anofox import quality
        result = quality.agg_check(conn, quality_table, "score", "max", upper_threshold=50.0)
        assert result["status"] == "fail"

    def test_unbounded_sides_pass(self, conn, quality_table):
        from anofox import quality
        result = quality.agg_check(conn, quality_table, "score", "min", lower_threshold=0.5)
        assert result["status"] == "pass"

    def test_raises_for_unknown_agg(self, conn, quality_table):
        from anofox import quality
        with pytest.raises(ValueError, match="agg must be one of"):
            quality.agg_check(conn, quality_table, "score", "variance")


class TestDuplicateCount:
    def test_unique_column_passes(self, conn, quality_table):
        from anofox import quality
        result = quality.duplicate_count(conn, quality_table, "id")
        assert result["status"] == "pass"

    def test_duplicated_column_fails(self, conn, quality_table):
        from anofox import quality
        result = quality.duplicate_count(conn, quality_table, "category")
        assert result["status"] == "fail"

    def test_allowance_and_list_columns(self, conn, quality_table):
        from anofox import quality
        result = quality.duplicate_count(conn, quality_table, ["id", "category"], max_duplicates=0)
        assert result["status"] == "pass"

    def test_raises_for_negative_allowance(self, conn, quality_table):
        from anofox import quality
        with pytest.raises(ValueError, match="max_duplicates must be >= 0"):
            quality.duplicate_count(conn, quality_table, "id", max_duplicates=-1)


class TestOccurrence:
    def test_max_within_bounds(self, conn, quality_table):
        from anofox import quality
        result = quality.occurrence(conn, quality_table, "category", "max", upper_threshold=25)
        assert result["status"] == "pass"

    def test_max_exceeds_upper(self, conn, quality_table):
        from anofox import quality
        result = quality.occurrence(conn, quality_table, "category", "max", upper_threshold=10)
        assert result["status"] == "fail"

    def test_raises_for_unknown_mode(self, conn, quality_table):
        from anofox import quality
        with pytest.raises(ValueError, match="mode must be"):
            quality.occurrence(conn, quality_table, "category", "median")


class TestMatchRate:
    @pytest.fixture(scope="class", autouse=True)
    def ref_table(self, conn):
        conn.execute(
            "CREATE OR REPLACE TABLE _qref AS SELECT unnest(range(1, 51)) AS ref_id"
        )
        yield "_qref"
        conn.execute("DROP TABLE IF EXISTS _qref")

    def test_partial_match_fails_default_threshold(self, conn, quality_table):
        from anofox import quality
        result = quality.match_rate(conn, quality_table, "_qref", "id", "ref_id")
        assert result["status"] == "fail"

    def test_partial_match_passes_relaxed_threshold(self, conn, quality_table):
        from anofox import quality
        result = quality.match_rate(conn, quality_table, "_qref", "id", "ref_id", min_rate=0.4)
        assert result["status"] == "pass"


class TestCompliance:
    def test_pass_when_all_rows_comply(self, conn, quality_table):
        from anofox import quality
        result = quality.compliance(conn, quality_table, "score > 0")
        assert result["status"] == "pass"

    def test_fail_below_min_rate(self, conn, quality_table):
        from anofox import quality
        result = quality.compliance(conn, quality_table, "score > 90")
        assert result["status"] == "fail"

    def test_expression_with_quotes(self, conn, quality_table):
        from anofox import quality
        result = quality.compliance(conn, quality_table, "category LIKE 'cat%'")
        assert result["status"] == "pass"


@pytest.fixture(scope="module")
def daily_table(conn):
    """Two weeks of daily events with a collapse on the last day."""
    conn.execute(
        "CREATE OR REPLACE TABLE _daily AS "
        "SELECT d::DATE AS event_date, 'ok' AS status FROM ("
        "  SELECT d, unnest(range(1, CASE WHEN d = DATE '2026-03-14' THEN 2 ELSE 11 END)) AS r "
        "  FROM (SELECT unnest(generate_series(DATE '2026-03-01', DATE '2026-03-14', INTERVAL '1 day')) AS d)"
        ")"
    )
    yield "_daily"
    conn.execute("DROP TABLE IF EXISTS _daily")


class TestRelCountChange:
    def test_normal_day_passes(self, conn, daily_table):
        from anofox import quality
        result = quality.rel_count_change(conn, daily_table, "event_date", reference_date="2026-03-13")
        assert result["status"] == "pass"

    def test_collapse_day_fails(self, conn, daily_table):
        from anofox import quality
        result = quality.rel_count_change(conn, daily_table, "event_date", reference_date="2026-03-14")
        assert result["status"] == "fail"

    def test_unbounded_thresholds_pass(self, conn, daily_table):
        from anofox import quality
        result = quality.rel_count_change(
            conn, daily_table, "event_date",
            lower_threshold=None, upper_threshold=None, reference_date="2026-03-14",
        )
        assert result["status"] == "pass"


class TestMetricAnomalyIqr:
    def test_normal_day_passes(self, conn, daily_table):
        from anofox import quality
        result = quality.metric_anomaly_iqr(conn, daily_table, "event_date", reference_date="2026-03-13")
        assert result["status"] == "pass"

    def test_collapse_day_fails(self, conn, daily_table):
        from anofox import quality
        result = quality.metric_anomaly_iqr(conn, daily_table, "event_date", reference_date="2026-03-14")
        assert result["status"] == "fail"

    def test_upper_mode_ignores_collapse(self, conn, daily_table):
        from anofox import quality
        result = quality.metric_anomaly_iqr(
            conn, daily_table, "event_date", mode="upper", reference_date="2026-03-14"
        )
        assert result["status"] == "pass"

    def test_raises_for_unknown_mode(self, conn, daily_table):
        from anofox import quality
        with pytest.raises(ValueError, match="mode must be"):
            quality.metric_anomaly_iqr(conn, daily_table, "event_date", mode="sideways")


class TestRollingValuesInSet:
    def test_clean_window_passes(self, conn, daily_table):
        from anofox import quality
        result = quality.rolling_values_in_set(
            conn, daily_table, "status", ["ok"], "event_date", reference_date="2026-03-14"
        )
        assert result["status"] == "pass"

    def test_disallowed_value_fails(self, conn, daily_table):
        from anofox import quality
        result = quality.rolling_values_in_set(
            conn, daily_table, "status", ["done"], "event_date", reference_date="2026-03-14"
        )
        assert result["status"] == "fail"


class TestCheckSuite:
    @pytest.fixture(scope="class", autouse=True)
    def suite_tables(self, conn):
        conn.execute(
            "CREATE OR REPLACE TABLE _suite_orders AS SELECT * FROM (VALUES "
            "(1, 'completed', 10.0), (2, 'completed', 25.0), (3, 'pending', -4.0), (4, 'weird', 8.0)"
            ") t(order_id, status, amount)"
        )
        yield
        conn.execute("DROP TABLE IF EXISTS _suite_orders")
        conn.execute("DROP TABLE IF EXISTS _suite_checks")
        conn.execute("DROP TABLE IF EXISTS _suite_results")

    def _checks(self):
        return [
            {"check_name": "volume", "check_type": "volume", "table_name": "_suite_orders",
             "lower_threshold": 1, "upper_threshold": 100},
            {"check_name": "status_set", "check_type": "values_in_set", "table_name": "_suite_orders",
             "column_name": "status", "params": {"allowed_values": ["completed", "pending"]},
             "lower_threshold": 1.0},
            {"check_name": "amount_pos", "check_type": "compliance", "table_name": "_suite_orders",
             "params": {"expression": "amount > 0"}, "lower_threshold": 1.0, "monitor_only": True},
        ]

    def test_define_and_run(self, conn):
        from anofox import quality
        quality.define_checks(conn, self._checks(), "_suite_checks")
        results = quality.run_checks(conn, "_suite_checks")
        by_name = {r["check_name"]: r for r in results}
        assert by_name["volume"]["status"] == "pass"
        assert by_name["status_set"]["status"] == "fail"
        assert by_name["amount_pos"]["status"] == "warn"  # monitor_only downgrades fail
        assert set(results[0].keys()) >= {
            "run_ts", "check_name", "check_type", "table_name", "column_name",
            "identifier", "value", "lower_threshold", "upper_threshold", "status", "message",
        }

    def test_persistence_appends(self, conn):
        from anofox import quality
        quality.define_checks(conn, self._checks(), "_suite_checks")
        quality.run_checks(conn, "_suite_checks", result_table="_suite_results")
        quality.run_checks(conn, "_suite_checks", result_table="_suite_results")
        count = conn.execute("SELECT COUNT(*) FROM _suite_results").fetchall()[0][0]
        assert count == 6

    def test_missing_required_key_raises(self, conn):
        from anofox import quality
        with pytest.raises(ValueError, match="check_name"):
            quality.define_checks(conn, [{"check_type": "volume", "table_name": "_suite_orders"}])
