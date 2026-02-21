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
