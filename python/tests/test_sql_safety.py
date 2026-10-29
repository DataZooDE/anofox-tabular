"""
Values and names that need SQL quoting must reach the extension unchanged.

The wrappers bind values as query parameters and quote identifiers, so an
apostrophe in an email, a quote in a column name or a table name with special
characters neither crashes the parser nor changes the statement.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")

ODD_TABLE = "it's"


@pytest.fixture(scope="module")
def odd_table(conn):
    quoted = '"' + ODD_TABLE.replace('"', '""') + '"'
    conn.execute(
        f"CREATE OR REPLACE TABLE {quoted} AS "
        "SELECT i AS \"o'id\", i * 1.0 AS \"a\"\"b\", 'x' || (i % 3) AS s, "
        "TIMESTAMP '2026-01-01' + INTERVAL (i) HOUR AS ts "
        "FROM range(1, 51) t(i)"
    )
    yield ODD_TABLE
    conn.execute(f"DROP TABLE IF EXISTS {quoted}")


class TestScalarValues:
    def test_email_with_apostrophe(self, conn):
        from anofox import validate
        assert validate.email_is_valid(conn, "o'brien@example.ie") is True

    def test_email_validate_with_apostrophe(self, conn):
        from anofox import validate
        assert isinstance(validate.email_validate(conn, "o'brien@example.ie"), dict)

    def test_phone_functions_with_apostrophe(self, conn):
        from anofox import validate
        assert validate.phone_is_valid(conn, "+1 415' 555 2671", region="US") in (True, False)
        assert isinstance(validate.phone_parse(conn, "it's 4155552671"), dict)
        assert isinstance(validate.phone_format(conn, "+14155552671", "US", "E164"), str)
        # Unparsable: the value reached the extension and came back as NULL.
        assert validate.phone_region(conn, "+1 '415", "US") is None

    def test_mode_is_bound_not_pasted(self, conn):
        from anofox import validate
        import duckdb
        # A quote in the mode reaches the extension as a value, never as SQL text.
        try:
            validate.email_is_valid(conn, "a@b.com", mode="re'gex")
        except duckdb.ParserException:  # pragma: no cover - the bug
            pytest.fail("mode was pasted into the SQL text")
        except duckdb.Error:
            pass

    def test_vat_with_apostrophe(self, conn):
        from anofox import vat
        assert vat.vat_is_valid(conn, "DE'111111125") in (True, False)
        assert vat.vat_is_valid_syntax(conn, "D'E") is False
        assert isinstance(vat.vat_normalize(conn, "de 'x"), str)
        assert vat.is_valid_vat_country(conn, "D'") is False
        assert vat.vat_exists(conn, "D'") in (True, False)
        assert vat.vat_is_eu_member(conn, "D'") is False
        assert vat.vat_country_name(conn, "D'") is None
        assert vat.vat_split(conn, "it's") is None or isinstance(vat.vat_split(conn, "it's"), dict)
        assert isinstance(vat.make_vat(conn, "DE111111125"), dict)

    def test_money_with_apostrophe(self, conn):
        from anofox import money
        import duckdb
        assert money.is_valid_currency(conn, "U'S") is False
        for call in (
            lambda: money.currency_symbol(conn, "U'S"),
            lambda: money.currency_name(conn, "U'S"),
            lambda: money.make_money(conn, 1.5, "U'S"),
            lambda: money.money_format(conn, {"amount": 1, "currency": "USD"}, "it's"),
        ):
            try:
                call()
            except duckdb.ParserException:  # pragma: no cover - the bug
                pytest.fail("value was pasted into the SQL text")
            except duckdb.Error:
                pass

    def test_pii_mask_strategy_is_bound(self, conn):
        from anofox import pii
        import duckdb
        with pytest.raises(duckdb.Error) as exc:
            pii.pii_mask(conn, "mail a@b.com", strategy="red'act")
        assert not isinstance(exc.value, duckdb.ParserException)


class TestDataFrameColumns:
    def test_email_column_with_double_quote(self, conn):
        from anofox import validate
        df = pd.DataFrame({'e"mail': ["a@b.com", "o'brien@example.ie", "bad"]})
        out = validate.email_is_valid(conn, df, column='e"mail')
        assert list(out["email_is_valid"]) == [True, True, False]

    def test_phone_column_with_double_quote(self, conn):
        from anofox import validate
        df = pd.DataFrame({'p"h': ["+14155552671", "x"]})
        out = validate.phone_is_valid(conn, df, column='p"h', region="US")
        assert list(out["phone_is_valid"]) == [True, False]

    def test_pii_detect_column_with_double_quote(self, conn):
        from anofox import pii
        df = pd.DataFrame({'t"xt': ["mail a@b.com"]})
        out = pii.pii_detect(conn, df, column='t"xt')
        assert "pii_detect" in out.columns


class TestTableNames:
    def test_quality_checks(self, conn, odd_table):
        from anofox import quality
        assert quality.volume(conn, odd_table, min_rows=1)["status"] == "pass"
        assert quality.null_rate(conn, odd_table, "o'id")["status"] == "pass"
        assert quality.distinct_count(conn, odd_table, "s", min_distinct=1)["status"] == "pass"
        assert quality.zscore(conn, odd_table, 'a"b')["status"] == "pass"
        assert quality.iqr(conn, odd_table, 'a"b')["status"] == "pass"
        assert quality.schema_check(conn, odd_table, ["o'id", 'a"b'])["status"] == "pass"
        assert quality.regex_match(conn, odd_table, "s", "^x")["status"] == "pass"
        assert quality.values_in_set(conn, odd_table, "s", ["x0", "x1", "x2"])["status"] == "pass"
        assert quality.agg_check(conn, odd_table, 'a"b', "max", upper_threshold=100)["status"] == "pass"
        assert quality.duplicate_count(conn, odd_table, ["o'id"])["status"] == "pass"
        assert quality.occurrence(conn, odd_table, "s")["status"] in ("pass", "fail")
        assert quality.match_rate(conn, odd_table, odd_table, "o'id", "o'id")["status"] == "pass"
        assert quality.compliance(conn, odd_table, "s <> 'y'")["status"] == "pass"

    def test_time_aware_checks(self, conn, odd_table):
        from anofox import quality
        assert "status" in quality.rel_count_change(conn, odd_table, "ts")
        assert "status" in quality.metric_anomaly_iqr(conn, odd_table, "ts", 'a"b')
        assert "status" in quality.rolling_values_in_set(conn, odd_table, "s", ["x0"], "ts", min_rate=0.0)

    def test_profile_and_anomaly(self, conn, odd_table):
        from anofox import anomaly, profile
        assert len(profile.profile_table(conn, odd_table, columns=["o'id"])) == 1
        assert len(profile.profile_summary(conn, odd_table)) == 1
        assert anomaly.isolation_forest(conn, odd_table, 'a"b')["total_count"] == 50
        assert anomaly.dbscan(conn, odd_table, 'a"b')["total_count"] == 50
        out = anomaly.isolation_forest(conn, odd_table, 'a"b', output_mode="scores", id_column="o'id")
        assert "o'id" in out.columns

    def test_diff_and_pii(self, conn, odd_table):
        from anofox import diff, pii
        assert len(diff.joindiff(conn, odd_table, odd_table, primary_keys="o'id")) == 0
        assert len(diff.joindiff(conn, odd_table, odd_table, primary_keys="o'id", compare_columns=["s"])) == 0
        assert len(diff.hashdiff(conn, odd_table, odd_table, primary_keys=["o'id"])) == 0
        pii.pii_scan_table(conn, odd_table, columns=["s"])
        pii.pii_audit_table(conn, odd_table, columns=["s"], id_column="o'id")

    def test_connection_profile(self, conn, odd_table):
        out = conn.profile(odd_table)
        assert set(out["column"]) == {"o'id", 'a"b', "s", "ts"}

    def test_connection_validate_and_quality_check(self, conn):
        from anofox.validate import EmailRule, PhoneRule
        df = pd.DataFrame({'e"m': ["o'brien@example.ie", "bad"], "p'h": ["+14155552671", "x"]})
        result = conn.validate(df, {'e"m': EmailRule(), "p'h": PhoneRule(region="US")})
        assert len(result.failures) == 2
        qc = conn.quality_check(df, {"null_rate": {"column": 'e"m'}})
        assert qc["status"] == "pass"
