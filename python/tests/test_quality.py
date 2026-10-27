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


class TestTimeAwareDateRange:
    """start_date/end_date return one dict per reference date (#109)."""

    def test_rel_count_change_range_rows_match_single_dates(self, conn, daily_table):
        from anofox import quality
        rows = quality.rel_count_change(
            conn, daily_table, "event_date", start_date="2026-03-12", end_date="2026-03-14"
        )
        assert [str(r["reference_date"]) for r in rows] == ["2026-03-12", "2026-03-13", "2026-03-14"]
        assert [r["status"] for r in rows] == ["pass", "pass", "fail"]
        single = quality.rel_count_change(conn, daily_table, "event_date", reference_date="2026-03-14")
        assert single["status"] == rows[-1]["status"]
        assert single["_raw"] == list(rows[-1].values())

    def test_metric_anomaly_iqr_range(self, conn, daily_table):
        from anofox import quality
        rows = quality.metric_anomaly_iqr(
            conn, daily_table, "event_date", start_date="2026-03-13", end_date="2026-03-14"
        )
        assert [r["status"] for r in rows] == ["pass", "fail"]
        assert rows[1]["metric_value"] == 1.0

    def test_rolling_values_in_set_range(self, conn, daily_table):
        from anofox import quality
        rows = quality.rolling_values_in_set(
            conn, daily_table, "status", ["ok"], "event_date",
            start_date="2026-03-01", end_date="2026-03-03",
        )
        assert len(rows) == 3
        assert all(r["status"] == "pass" for r in rows)

    def test_range_requires_both_bounds(self, conn, daily_table):
        import duckdb
        from anofox import quality
        with pytest.raises(duckdb.Error, match="start_date and end_date must be given together"):
            quality.rel_count_change(conn, daily_table, "event_date", start_date="2026-03-01")

    def test_range_excludes_reference_date(self, conn, daily_table):
        import duckdb
        from anofox import quality
        with pytest.raises(duckdb.Error, match="reference_date cannot be combined"):
            quality.metric_anomaly_iqr(
                conn, daily_table, "event_date", reference_date="2026-03-05",
                start_date="2026-03-01", end_date="2026-03-02",
            )


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


class TestQualifiedCheckTables:
    """Schema-qualified checks and result tables land in that schema, not main."dq.checks"."""

    @pytest.fixture(autouse=True)
    def _schema(self, conn):
        conn.execute("CREATE SCHEMA IF NOT EXISTS dq")
        conn.execute("CREATE SCHEMA IF NOT EXISTS sales")
        conn.execute(
            "CREATE OR REPLACE TABLE sales.invoices AS SELECT * FROM (VALUES "
            "('INV-2026-0001', 'ACME GmbH', 1250.00), ('INV-2026-0002', 'Globex AG', NULL), "
            "('INV-2026-0003', 'Initech SE', 980.50)) t(invoice_no, customer, net_amount)"
        )
        yield
        conn.execute("DROP SCHEMA dq CASCADE")
        conn.execute("DROP SCHEMA sales CASCADE")
        conn.execute('DROP TABLE IF EXISTS main."dq.checks"')
        conn.execute('DROP TABLE IF EXISTS main."dq.results"')

    CHECKS = [
        {"check_name": "invoice_volume", "check_type": "volume", "table_name": "sales.invoices",
         "lower_threshold": 1},
        {"check_name": "net_amount_nulls", "check_type": "null_rate", "table_name": "sales.invoices",
         "column_name": "net_amount", "upper_threshold": 0.0},
    ]

    def _tables(self, conn):
        return sorted(conn.execute(
            "SELECT schema_name || '.' || table_name FROM duckdb_tables() "
            "WHERE table_name IN ('checks', 'results', 'dq.checks', 'dq.results')"
        ).fetchall())

    def test_define_and_run_in_schema(self, conn):
        from anofox import quality
        assert quality.define_checks(conn, self.CHECKS, "dq.checks") == "dq.checks"
        assert self._tables(conn) == [("dq.checks",)]
        results = {r["check_name"]: r["status"] for r in quality.run_checks(conn, "dq.checks")}
        assert results == {"invoice_volume": "pass", "net_amount_nulls": "fail"}

    def test_result_table_in_schema(self, conn):
        from anofox import quality
        quality.define_checks(conn, self.CHECKS, "dq.checks")
        quality.run_checks(conn, "dq.checks", result_table="dq.results")
        rows = quality.run_checks(conn, "dq.checks", result_table="dq.results")
        assert len(rows) == 2
        assert self._tables(conn) == [("dq.checks",), ("dq.results",)]
        assert conn.execute("SELECT count(*) FROM dq.results").fetchone()[0] == 4


class TestNamedResultFields:
    """Result dicts carry every result column by name, with floats instead of Decimal."""

    @staticmethod
    def _no_decimal(result):
        from decimal import Decimal
        return not any(isinstance(v, Decimal) for k, v in result.items() if k != "_raw")

    def test_null_rate_fields(self, conn, sparse_table):
        from anofox import quality
        result = quality.null_rate(conn, sparse_table, "val", max_null_rate=0.1)
        assert result["message"].startswith("Null rate")
        assert isinstance(result["null_rate"], float)
        assert result["max_null_rate"] == 0.1 and isinstance(result["max_null_rate"], float)
        assert isinstance(result["null_count"], int) and isinstance(result["total_count"], int)
        assert result["message"] == result["_raw"][-1]
        assert self._no_decimal(result)

    def test_volume_fields(self, conn, quality_table):
        from anofox import quality
        result = quality.volume(conn, quality_table, min_rows=10)
        assert result["row_count"] == 100
        assert result["min_rows"] == 10
        assert result["max_rows"] is None
        assert "message" in result

    def test_zscore_threshold_is_float(self, conn, quality_table):
        from anofox import quality
        result = quality.zscore(conn, quality_table, "score", threshold=3.0)
        assert result["threshold"] == 3.0 and isinstance(result["threshold"], float)
        assert self._no_decimal(result)

    def test_distinct_count_without_max(self, conn, quality_table):
        from anofox import quality
        result = quality.distinct_count(conn, quality_table, "category", min_distinct=2)
        assert result["distinct_count"] == 5
        assert result["max_distinct"] is None

    def test_range_rows_have_no_decimal(self, conn, sparse_table):
        from anofox import quality
        assert self._no_decimal(quality.iqr(conn, sparse_table, "val"))


class TestFreshness:
    """freshness works without pytz and takes the SQL reference_time."""

    @pytest.fixture
    def events(self, conn):
        conn.execute(
            "CREATE OR REPLACE TABLE _events AS "
            "SELECT TIMESTAMP '2026-01-30 10:00:00' - INTERVAL (i) HOUR AS created_at FROM range(5) t(i)"
        )
        yield "_events"
        conn.execute("DROP TABLE IF EXISTS _events")

    def test_against_now(self, conn, events):
        from anofox import quality
        result = quality.freshness(conn, events, "created_at", "1 day")
        assert result["status"] == "fail"
        assert result["age_seconds"] > 0

    def test_reference_time(self, conn, events):
        import datetime
        from anofox import quality
        result = quality.freshness(conn, events, "created_at", "1 day", reference_time="2026-01-31 08:00:00")
        assert result["status"] == "pass"
        assert result["age_seconds"] == 79200
        assert result["metric_value"] == datetime.datetime(2026, 1, 30, 10, 0)
        assert result["threshold"] == datetime.datetime(2026, 1, 30, 8, 0)


class TestDefineChecksDates:
    """define_checks serialises date/datetime/Decimal params so run_checks can use them."""

    @pytest.fixture(autouse=True)
    def orders(self, conn):
        conn.execute(
            "CREATE OR REPLACE TABLE _dated_orders AS "
            "SELECT DATE '2026-03-01' + (i % 10)::INTEGER AS order_date, i AS order_id "
            "FROM range(1, 101) t(i)"
        )
        # A spike on the latest day: only a check pinned to an earlier date passes.
        conn.execute(
            "INSERT INTO _dated_orders SELECT DATE '2026-03-10', i FROM range(101, 151) t(i)"
        )
        yield
        conn.execute("DROP TABLE IF EXISTS _dated_orders")
        conn.execute("DROP TABLE IF EXISTS _dated_checks")

    def test_date_param_runs_end_to_end(self, conn):
        import datetime
        from decimal import Decimal
        from anofox import quality
        quality.define_checks(conn, [{
            "check_name": "orders_week", "check_type": "rel_count_change",
            "table_name": "_dated_orders",
            "params": {"date_column": "order_date", "window_days": 3,
                       "reference_date": datetime.date(2026, 3, 8)},
            "lower_threshold": float(Decimal("-0.5")), "upper_threshold": 0.5,
        }, {
            "check_name": "fresh", "check_type": "freshness", "table_name": "_dated_orders",
            "column_name": "order_date",
            "params": {"reference_time": datetime.datetime(2026, 3, 10, 6, 0)},
            "upper_threshold": float(Decimal("86400")),
        }], "_dated_checks")
        stored = conn.execute("SELECT params FROM _dated_checks ORDER BY check_name").fetchall()
        assert '"reference_time": "2026-03-10T06:00:00"' in stored[0][0]
        assert '"reference_date": "2026-03-08"' in stored[1][0]
        results = {r["check_name"]: r for r in quality.run_checks(conn, "_dated_checks")}
        assert results["orders_week"]["status"] == "pass", results["orders_week"]["message"]
        assert results["fresh"]["status"] == "pass", results["fresh"]["message"]
        assert results["fresh"]["value"] == 21600.0


class TestTempTableNamesHidden:
    """Errors for a DataFrame input do not show the generated temporary table name."""

    def _orders(self):
        import pandas as pd
        return pd.DataFrame({"order_id": [1, 2, 3], "amount": [19.99, 5.0, 120.0]})

    def test_table_function_error(self, conn):
        from anofox import profile
        with pytest.raises(Exception) as info:
            profile.profile_table(conn, self._orders(), columns=["amout"])
        assert "_anofox_" not in str(info.value)
        assert "the DataFrame" in str(info.value)
        assert "amout" in str(info.value)

    def test_anomaly_error(self, conn):
        from anofox import anomaly
        with pytest.raises(Exception) as info:
            anomaly.isolation_forest(conn, self._orders(), "amout")
        assert "_anofox_" not in str(info.value)
        assert "the DataFrame" in str(info.value)

    def test_error_type_kept(self, conn):
        import duckdb
        from anofox import anomaly
        with pytest.raises(duckdb.BinderException):
            anomaly.isolation_forest(conn, self._orders(), "amout")

    def test_table_name_unchanged(self, conn):
        from anofox import anomaly
        conn.execute("CREATE OR REPLACE TABLE _named_orders AS SELECT 1.0 AS amount")
        try:
            with pytest.raises(Exception, match="_named_orders"):
                anomaly.isolation_forest(conn, "_named_orders", "amout")
        finally:
            conn.execute("DROP TABLE IF EXISTS _named_orders")

    def test_result_message(self, conn):
        from anofox import quality
        result = quality.schema_check(conn, self._orders(), ["order_id", "customer_id"])
        assert result["status"] == "fail"
        assert result["message"] == "The DataFrame is missing 1 column(s)"

    def test_diff_names_source_and_target(self, conn):
        import pandas as pd
        from anofox import diff
        before = pd.DataFrame({"sku": ["A-100", "B-200"], "price": [9.99, 24.5]})
        after = pd.DataFrame({"article": ["A-100", "B-200"], "price": [9.99, 26.0]})
        with pytest.raises(Exception) as info:
            diff.joindiff(conn, before, after, primary_keys="sku")
        assert "not found in the target DataFrame" in str(info.value)
        assert "_anofox_" not in str(info.value)

    def test_quality_check_error(self, conn):
        with pytest.raises(Exception) as info:
            conn.quality_check(self._orders(), {"null_rate": {"column": "amout"}})
        assert "_anofox_" not in str(info.value)

    def test_no_temp_table_left_behind(self, conn):
        from anofox import diff
        import pandas as pd
        before = pd.DataFrame({"sku": ["A-100"], "price": [9.99]})
        with pytest.raises(TypeError):
            diff.joindiff(conn, before, ["not", "a", "frame"], primary_keys="sku")
        leftover = conn.execute(
            "SELECT count(*) FROM duckdb_tables() WHERE table_name LIKE '\\_anofox\\_%' ESCAPE '\\'"
        ).fetchall()[0][0]
        assert leftover == 0


class TestAwareReferenceTimes:
    """
    A time zone aware reference time is the instant it names: it is bound as naive
    UTC, whatever the session TimeZone. A naive datetime (and text without an
    offset) is UTC, like in the SQL functions.
    """

    @pytest.fixture
    def berlin(self, conn):
        previous = conn.execute("SELECT current_setting('TimeZone')").fetchone()[0]
        conn.execute("SET TimeZone = 'Europe/Berlin'")
        yield
        conn.execute(f"SET TimeZone = '{previous}'")

    @pytest.fixture(params=["pandas", "polars"])
    def shipments(self, request):
        import datetime
        rows = {
            "shipment_id": ["SHP-1001", "SHP-1002", "SHP-1003"],
            "carrier": ["DHL", "UPS", "DPD"],
            "scanned_at": [  # naive UTC, newest 2026-07-14 10:00 UTC
                datetime.datetime(2026, 7, 14, 6, 15),
                datetime.datetime(2026, 7, 14, 8, 40),
                datetime.datetime(2026, 7, 14, 10, 0),
            ],
        }
        if request.param == "pandas":
            import pandas as pd
            return pd.DataFrame(rows)
        import polars as pl
        return pl.DataFrame(rows)

    @pytest.mark.parametrize("reference", [
        "aware_utc", "aware_plus_two", "naive_utc", "pandas_aware", "iso_text_with_offset",
    ])
    def test_freshness_reference_time_is_an_instant(self, conn, berlin, shipments, reference):
        import datetime
        import pandas as pd
        from anofox import quality
        utc_noon = datetime.datetime(2026, 7, 14, 12, 0, tzinfo=datetime.timezone.utc)
        value = {
            "aware_utc": utc_noon,
            "aware_plus_two": utc_noon.astimezone(datetime.timezone(datetime.timedelta(hours=2))),
            "naive_utc": utc_noon.replace(tzinfo=None),
            "pandas_aware": pd.Timestamp(utc_noon).tz_convert("Europe/Berlin"),
            "iso_text_with_offset": "2026-07-14T14:00:00+02:00",
        }[reference]
        result = quality.freshness(conn, shipments, "scanned_at", "3 hours", reference_time=value)
        assert result["age_seconds"] == 7200  # 2 h old, not 2 h + the Berlin offset
        assert result["status"] == "pass"
        assert result["threshold"] == datetime.datetime(2026, 7, 14, 9, 0)

    def test_reference_date_from_aware_datetime_is_its_utc_date(self, conn, berlin, daily_table):
        import datetime
        from anofox import quality
        # 01:00 in Berlin (CET, +01) on 2026-03-14 is 2026-03-13 in UTC: a normal day.
        berlin_time = datetime.datetime(
            2026, 3, 14, 0, 30, tzinfo=datetime.timezone(datetime.timedelta(hours=1))
        )
        result = quality.rel_count_change(conn, daily_table, "event_date", reference_date=berlin_time)
        assert result["status"] == "pass"
        rows = quality.rel_count_change(
            conn, daily_table, "event_date", start_date=berlin_time, end_date=berlin_time
        )
        assert [r["reference_date"] for r in rows] == [datetime.date(2026, 3, 13)]

    def test_define_checks_stores_aware_datetime_as_utc(self, conn, berlin):
        import datetime
        from anofox import quality
        conn.execute(
            "CREATE OR REPLACE TABLE _scans AS SELECT * FROM (VALUES "
            "('SHP-1001', TIMESTAMP '2026-07-14 08:40:00'), ('SHP-1002', TIMESTAMP '2026-07-14 10:00:00')"
            ") t(shipment_id, scanned_at)"
        )
        try:
            quality.define_checks(conn, [{
                "check_name": "scans_fresh", "check_type": "freshness", "table_name": "_scans",
                "column_name": "scanned_at",
                "params": {"reference_time": datetime.datetime(
                    2026, 7, 14, 14, 0, tzinfo=datetime.timezone(datetime.timedelta(hours=2)))},
                "upper_threshold": 10800.0,
            }], "_scan_checks")
            stored = conn.execute("SELECT params FROM _scan_checks").fetchone()[0]
            assert '"reference_time": "2026-07-14T12:00:00"' in stored
            [result] = quality.run_checks(conn, "_scan_checks")
            assert result["value"] == 7200.0
            assert result["status"] == "pass", result["message"]
        finally:
            conn.execute("DROP TABLE IF EXISTS _scans")
            conn.execute("DROP TABLE IF EXISTS _scan_checks")
