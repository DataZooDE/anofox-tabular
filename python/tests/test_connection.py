"""
Tests for _connection.py and __init__.connect().

These tests require the extension binary (skipped via conftest if absent).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


class TestConnect:
    def test_connect_returns_anofox_connection(self, ext_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        import anofox
        conn = anofox.connect(extension_path=ext_path)
        try:
            from anofox._connection import AnofoxConnection
            assert isinstance(conn, AnofoxConnection)
        finally:
            conn.close()

    def test_context_manager(self, ext_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        import anofox
        with anofox.connect(extension_path=ext_path) as conn:
            result = conn.execute("SELECT 1+1").fetchone()
        assert result[0] == 2

    def test_extension_is_loaded(self, conn):
        assert conn.is_extension_loaded()

    def test_anofox_functions_registered(self, conn):
        result = conn.execute(
            "SELECT count(*) FROM duckdb_functions() WHERE function_name LIKE 'anofox%'"
        ).fetchone()
        assert result[0] > 0

    def test_execute_simple_query(self, conn):
        result = conn.execute("SELECT 'hello' AS greeting").fetchone()
        assert result[0] == "hello"

    def test_native_property_is_duckdb_conn(self, conn):
        import duckdb
        assert conn.native is not None

    def test_env_var_extension_path(self, ext_path, monkeypatch):
        if ext_path is None:
            pytest.skip("No extension binary")
        import anofox
        monkeypatch.setenv("ANOFOX_EXT_PATH", ext_path)
        # connect without explicit extension_path — should pick up env var
        conn = anofox.connect()
        try:
            assert conn.is_extension_loaded()
        finally:
            conn.close()


class TestProfile:
    def test_profile_returns_dataframe(self, conn):
        import pandas as pd
        conn.execute("CREATE OR REPLACE TABLE _test_profile AS SELECT 1 AS a, 'x' AS b")
        result = conn.profile("_test_profile")
        assert isinstance(result, pd.DataFrame)
        assert "column" in result.columns
        assert "null_rate" in result.columns
        conn.execute("DROP TABLE IF EXISTS _test_profile")

    def test_profile_with_dataframe_input(self, conn):
        import pandas as pd
        df = pd.DataFrame({"score": [1.0, 2.0, None], "name": ["a", "b", "c"]})
        result = conn.profile(df)
        assert isinstance(result, pd.DataFrame)
        assert len(result) == 2  # two columns

    def test_profile_includes_null_rate(self, conn):
        import pandas as pd
        df = pd.DataFrame({"x": [1, None, None, None]})
        result = conn.profile(df)
        row = result[result["column"] == "x"].iloc[0]
        assert abs(row["null_rate"] - 0.75) < 0.01


class TestQualityCheckRules:
    def test_unknown_rule_raises(self, conn):
        import pandas as pd
        df = pd.DataFrame({"a": [1, None]})
        with pytest.raises(ValueError) as exc:
            conn.quality_check(df, {"null-rate": {"column": "a"}, "zscore": {"column": "a"}})
        message = str(exc.value)
        assert "null-rate" in message and "zscore" in message
        assert "volume" in message and "null_rate" in message and "distinct_count" in message

    def test_unknown_rule_creates_no_table(self, conn):
        import pandas as pd
        before = conn.execute(
            "SELECT count(*) FROM information_schema.tables WHERE table_name LIKE '_anofox_%'"
        ).fetchone()[0]
        with pytest.raises(ValueError):
            conn.quality_check(pd.DataFrame({"a": [1]}), {"nope": {}})
        after = conn.execute(
            "SELECT count(*) FROM information_schema.tables WHERE table_name LIKE '_anofox_%'"
        ).fetchone()[0]
        assert after == before

    def test_known_rules_still_run(self, conn):
        import pandas as pd
        result = conn.quality_check(pd.DataFrame({"a": [1, None]}), {"null_rate": {"column": "a"}})
        assert result["status"] == "fail"
        assert set(result["checks"]) == {"null_rate"}


class TestPendingResults:
    """
    A wrapper call must leave no unfinished query result behind, and must not
    trip over one left by the caller. The DuckDB 2.0 client refuses
    register(), unregister() and sql() while a result of execute() is not read
    to its end (fetchone() on a one-row result does not end it).
    """

    @pytest.fixture
    def fresh(self, ext_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        import anofox
        conn = anofox.connect(extension_path=ext_path)
        yield conn
        conn.close()

    def test_scalar_wrapper_then_dataframe_wrapper(self, fresh):
        import pandas as pd
        from anofox import money, validate, vat

        assert money.currency_name(fresh, "EUR")
        assert vat.vat_is_valid(fresh, "DE123456789") in (True, False)
        df = pd.DataFrame({"email": ["a@example.com", "bad"]})
        out = validate.email_is_valid(fresh, df, column="email")
        assert list(out["email_is_valid"]) == [True, False]

    def test_connection_methods_after_partly_read_result(self, fresh):
        import pandas as pd
        from anofox.validate import EmailRule

        fresh.execute("SELECT * FROM range(3)").fetchone()
        assert fresh.is_extension_loaded()
        df = pd.DataFrame({"email": ["a@example.com", "bad"]})
        fresh.execute("SELECT * FROM range(3)").fetchone()
        assert not fresh.validate(df, {"email": EmailRule()}).passed
        fresh.execute("SELECT * FROM range(3)").fetchone()
        assert len(fresh.profile(df)) == 1

    def test_register_and_sql_after_partly_read_result(self, fresh):
        import pandas as pd

        fresh.execute("SELECT 42").fetchone()
        fresh.register("pending_v", pd.DataFrame({"a": [1, 2]}))
        fresh.execute("SELECT * FROM range(3)").fetchone()
        assert fresh.sql("SELECT sum(a) FROM pending_v").fetchall() == [(3,)]
        fresh.execute("SELECT * FROM range(3)").fetchone()
        fresh.unregister("pending_v")


class TestValidateRules:
    """conn.validate() rejects rule objects it cannot check instead of skipping them."""

    @staticmethod
    def _contacts():
        import pandas as pd
        return pd.DataFrame({
            "customer_id": [1001, 1002, 1003],
            "email": ["ana.lopez@example.com", "bad-email", "j.meyer@example.org"],
            "phone": ["+49 30 901820", "123", "+1 650-253-0000"],
        })

    @pytest.mark.parametrize("rule", ["email", "phone", None, 42, {"mode": "regex"}])
    def test_unsupported_rule_raises(self, conn, rule):
        with pytest.raises(TypeError, match=r"'email'.*EmailRule.*PhoneRule"):
            conn.validate(self._contacts(), {"email": rule})

    def test_message_shows_the_rule(self, conn):
        with pytest.raises(TypeError, match=r"got 'email'"):
            conn.validate(self._contacts(), {"email": "email"})

    def test_bad_rule_checks_nothing(self, conn):
        from anofox.validate import EmailRule
        # The unsupported rule is reported even after a supported one.
        with pytest.raises(TypeError, match="'phone'"):
            conn.validate(self._contacts(), {"email": EmailRule(), "phone": "phone"})

    def test_supported_rules_still_work(self, conn):
        from anofox.validate import EmailRule, PhoneRule
        result = conn.validate(self._contacts(), {"email": EmailRule(), "phone": PhoneRule(region="DE")})
        assert result.passed is False
        assert sorted(result.failures["column"].tolist()) == ["email", "phone"]


class TestPhoneRuleDefaultRegion:
    """PhoneRule() leaves the region to anofox_tab_phonenumber_default_region,
    like the phone helpers (it used to hard-code 'US')."""

    def test_default_region_follows_the_setting(self, conn):
        import pandas as pd
        from anofox.validate import PhoneRule

        assert PhoneRule().region is None
        conn.execute("SET anofox_tab_phonenumber_default_region = 'DE'").fetchall()
        try:
            df = pd.DataFrame({"phone": ["030 12345678"]})
            assert conn.validate(df, {"phone": PhoneRule()}).passed
        finally:
            conn.execute("RESET anofox_tab_phonenumber_default_region").fetchall()


class TestPhoneRuleRegionColumn:
    """PhoneRule(region_column=...) takes each row's region from a column,
    like validate.phone_is_valid(region_column=...)."""

    @staticmethod
    def _customers():
        import pandas as pd
        return pd.DataFrame({
            "customer_id": [2001, 2002, 2003, 2004],
            "country": ["DE", "US", "GB", "DE"],
            "phone": ["030 12345678", "(650) 253-0000", "020 7946 0018", "12"],
        }, index=[10, 11, 12, 13])

    def test_region_per_row(self, conn):
        from anofox.validate import PhoneRule
        result = conn.validate(self._customers(), {"phone": PhoneRule(region_column="country")})
        assert result.passed is False
        assert result.failures["row_id"].tolist() == [13]
        assert result.failures["value"].tolist() == ["12"]
        assert result.failures["rule"].tolist() == ["phone(region_column=country)"]

    def test_one_region_fails_the_foreign_numbers(self, conn):
        from anofox.validate import PhoneRule
        result = conn.validate(self._customers(), {"phone": PhoneRule(region="US")})
        assert result.failures["row_id"].tolist() == [10, 12, 13]

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox.validate import PhoneRule
        result = conn.validate(pl.from_pandas(self._customers()),
                               {"phone": PhoneRule(region_column="country")})
        assert result.failures["row_id"].to_list() == [3]

    def test_region_and_region_column_are_exclusive(self):
        from anofox.validate import PhoneRule
        with pytest.raises(ValueError, match="region.*region_column"):
            PhoneRule(region="DE", region_column="country")

    def test_missing_region_column_is_named(self, conn):
        from anofox.validate import PhoneRule
        with pytest.raises(Exception, match="country_code"):
            conn.validate(self._customers(), {"phone": PhoneRule(region_column="country_code")})


class TestProfileSchemas:
    """conn.profile('t') profiles the table the name resolves to, not every table named 't'."""

    @pytest.fixture
    def two_schemas(self, conn):
        conn.execute("CREATE SCHEMA IF NOT EXISTS staging")
        conn.execute(
            "CREATE OR REPLACE TABLE customers AS SELECT * FROM (VALUES "
            "(1, 'ana@example.com'), (2, NULL), (3, 'carla@example.org')) t(customer_id, email)"
        )
        conn.execute(
            "CREATE OR REPLACE TABLE staging.customers AS SELECT * FROM (VALUES "
            "(1, 'Ana', 'DE', DATE '2026-01-03'), (2, 'Bob', NULL, NULL)) "
            "t(customer_id, full_name, country, signup_date)"
        )
        yield
        conn.execute("DROP TABLE IF EXISTS customers")
        conn.execute("DROP TABLE IF EXISTS staging.customers")
        conn.execute("DROP SCHEMA IF EXISTS staging")

    def test_plain_name_uses_search_path(self, conn, two_schemas):
        result = conn.profile("customers")
        assert result["column"].tolist() == ["customer_id", "email"]
        assert result["row_count"].tolist() == [3, 3]
        assert result[result["column"] == "email"]["null_count"].iloc[0] == 1

    def test_schema_qualified_name(self, conn, two_schemas):
        result = conn.profile("staging.customers")
        assert result["column"].tolist() == ["customer_id", "full_name", "country", "signup_date"]
        assert result["row_count"].tolist() == [2] * 4
        assert result[result["column"] == "country"]["null_count"].iloc[0] == 1

    def test_unknown_table_raises_catalog_error(self, conn, two_schemas):
        import duckdb
        with pytest.raises(duckdb.CatalogException):
            conn.profile("staging.orders")
