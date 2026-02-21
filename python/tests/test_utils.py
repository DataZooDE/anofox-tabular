"""
Tests for _utils.py — no extension binary required.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from anofox._utils import detect_df_type, register_df_as_table, relation_to_dataframe

pandas = pytest.importorskip("pandas")
polars = pytest.importorskip("polars")
duckdb = pytest.importorskip("duckdb")


# ---------------------------------------------------------------------------
# detect_df_type
# ---------------------------------------------------------------------------

class TestDetectDfType:
    def test_pandas_dataframe(self):
        import pandas as pd
        df = pd.DataFrame({"a": [1, 2, 3]})
        assert detect_df_type(df) == "pandas"

    def test_polars_dataframe(self):
        import polars as pl
        df = pl.DataFrame({"a": [1, 2, 3]})
        assert detect_df_type(df) == "polars"

    def test_raises_for_plain_dict(self):
        with pytest.raises(TypeError, match="pandas or polars"):
            detect_df_type({"a": [1, 2, 3]})

    def test_raises_for_list(self):
        with pytest.raises(TypeError, match="pandas or polars"):
            detect_df_type([[1, 2], [3, 4]])

    def test_raises_for_string(self):
        with pytest.raises(TypeError):
            detect_df_type("my_table")

    def test_raises_for_none(self):
        with pytest.raises(TypeError):
            detect_df_type(None)


# ---------------------------------------------------------------------------
# register_df_as_table
# ---------------------------------------------------------------------------

class TestRegisterDfAsTable:
    def _make_conn(self):
        return duckdb.connect(":memory:")

    def test_registers_pandas_df(self):
        import pandas as pd
        conn = self._make_conn()
        df = pd.DataFrame({"x": [1, 2, 3], "y": ["a", "b", "c"]})
        name = register_df_as_table(conn, df)
        assert isinstance(name, str)
        result = conn.execute(f"SELECT count(*) FROM {name}").fetchone()
        assert result[0] == 3

    def test_registers_polars_df(self):
        import polars as pl
        conn = self._make_conn()
        df = pl.DataFrame({"x": [10, 20], "y": [1.5, 2.5]})
        name = register_df_as_table(conn, df)
        result = conn.execute(f"SELECT count(*) FROM {name}").fetchone()
        assert result[0] == 2

    def test_custom_name(self):
        import pandas as pd
        conn = self._make_conn()
        df = pd.DataFrame({"a": [1]})
        name = register_df_as_table(conn, df, name="my_table")
        assert name == "my_table"
        result = conn.execute("SELECT count(*) FROM my_table").fetchone()
        assert result[0] == 1

    def test_generated_name_is_unique(self):
        import pandas as pd
        conn = self._make_conn()
        df = pd.DataFrame({"a": [1]})
        name1 = register_df_as_table(conn, df)
        name2 = register_df_as_table(conn, df)
        assert name1 != name2


# ---------------------------------------------------------------------------
# relation_to_dataframe
# ---------------------------------------------------------------------------

class TestRelationToDataframe:
    def _make_conn(self):
        return duckdb.connect(":memory:")

    def test_to_pandas(self):
        import pandas as pd
        conn = self._make_conn()
        rel = conn.sql("SELECT 1 AS x, 'hello' AS y")
        result = relation_to_dataframe(rel, "pandas")
        assert isinstance(result, pd.DataFrame)
        assert list(result.columns) == ["x", "y"]
        assert result["x"].iloc[0] == 1

    def test_to_polars(self):
        import polars as pl
        conn = self._make_conn()
        rel = conn.sql("SELECT 42 AS n")
        result = relation_to_dataframe(rel, "polars")
        assert isinstance(result, pl.DataFrame)
        assert result["n"][0] == 42

    def test_pandas_roundtrip(self):
        import pandas as pd
        conn = self._make_conn()
        original = pd.DataFrame({"col": [1, 2, 3]})
        name = register_df_as_table(conn, original)
        rel = conn.sql(f"SELECT * FROM {name}")
        restored = relation_to_dataframe(rel, "pandas")
        assert list(restored["col"]) == [1, 2, 3]

    def test_polars_roundtrip(self):
        import polars as pl
        conn = self._make_conn()
        original = pl.DataFrame({"val": [10, 20, 30]})
        name = register_df_as_table(conn, original)
        rel = conn.sql(f"SELECT * FROM {name}")
        restored = relation_to_dataframe(rel, "polars")
        assert restored["val"].to_list() == [10, 20, 30]
