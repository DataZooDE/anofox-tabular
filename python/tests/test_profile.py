"""
Tests for profile.py — assert Python API contract (return types, schema, numeric ranges).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def profile_table_fixture(conn):
    """Regular (non-temp) table visible to internal connections opened by the extension."""
    conn.execute(
        "CREATE OR REPLACE TABLE _profile_test AS "
        "SELECT * FROM (VALUES "
        "  (1, 'Alice', 'alice@example.com', 30,   75000.0, true,  TIMESTAMPTZ '2023-01-15'), "
        "  (2, 'Bob',   'bob@example.com',   25,   55000.0, false, TIMESTAMPTZ '2023-06-01'), "
        "  (3, 'Carol', NULL,                40,   90000.0, true,  TIMESTAMPTZ '2022-12-01'), "
        "  (4, NULL,    'dave@example.com',  NULL, NULL,    NULL,  NULL), "
        "  (5, 'Eve',   'not-an-email',      35,   65000.0, true,  TIMESTAMPTZ '2024-01-01') "
        ") t(id, name, email, age, salary, active, created_at)"
    )
    yield "_profile_test"
    conn.execute("DROP TABLE IF EXISTS _profile_test")


@pytest.fixture(scope="module")
def complex_table_fixture(conn):
    """Table with complex types (LIST, MAP, STRUCT)."""
    conn.execute(
        "CREATE OR REPLACE TABLE _complex_test AS "
        "SELECT * FROM (VALUES "
        "  (1, [1,2,3]::INTEGER[],        MAP {'a':1,'b':2},      {'city': 'Berlin', 'zip': '10115'}), "
        "  (2, [10,20]::INTEGER[],        MAP {'x':99},           {'city': 'Munich', 'zip': '80331'}), "
        "  (3, []::INTEGER[],             MAP {}::MAP(VARCHAR,INTEGER), {'city': 'Hamburg', 'zip': '20095'}), "
        "  (4, NULL::INTEGER[],           NULL::MAP(VARCHAR,INTEGER),  NULL::STRUCT(city VARCHAR, zip VARCHAR)), "
        "  (5, [7,8,9,10,11]::INTEGER[], MAP {'p':1,'q':2,'r':3}, {'city': 'Frankfurt', 'zip': '60311'}) "
        ") t(id, tags, metadata, address)"
    )
    yield "_complex_test"
    conn.execute("DROP TABLE IF EXISTS _complex_test")


class TestProfileTable:
    def test_returns_dataframe(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture)
        import pandas as pd
        assert isinstance(result, pd.DataFrame)

    def test_one_row_per_column(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture)
        assert len(result) == 7  # id, name, email, age, salary, active, created_at

    def test_expected_column_names(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture)
        expected = {
            "column_name", "column_type", "row_count", "null_count", "null_rate",
            "distinct_count", "distinct_rate", "min_val", "max_val",
            "mean", "median", "stddev", "p25", "p75", "skewness", "kurtosis",
            "top_values", "avg_length", "min_length", "max_length",
            "pattern_summary", "is_unique", "is_constant",
            "zero_count", "negative_count", "is_sampled", "actual_sample_size",
        }
        assert expected.issubset(set(result.columns))

    def test_columns_filter(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture, columns=["id", "age"])
        assert len(result) == 2
        assert set(result["column_name"]) == {"id", "age"}

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import profile
        df = pd.DataFrame({"x": [1, 2, 3, None], "y": ["a", "b", "c", "d"]})
        result = profile.profile_table(conn, df)
        assert isinstance(result, pd.DataFrame)
        assert len(result) == 2

    def test_null_count(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture)
        email_row = result[result["column_name"] == "email"].iloc[0]
        assert email_row["null_count"] == 1

    def test_numeric_mean(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, profile_table_fixture)
        age_row = result[result["column_name"] == "age"].iloc[0]
        assert abs(age_row["mean"] - 32.5) < 0.01

    def test_raises_for_invalid_input(self, conn):
        from anofox import profile
        with pytest.raises(TypeError, match="pandas or polars"):
            profile.profile_table(conn, [1, 2, 3])


class TestProfileTableComplexTypes:
    def test_pattern_summary_list(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        tags_row = result[result["column_name"] == "tags"].iloc[0]
        assert tags_row["pattern_summary"] == "list"

    def test_pattern_summary_map(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        meta_row = result[result["column_name"] == "metadata"].iloc[0]
        assert meta_row["pattern_summary"] == "map"

    def test_pattern_summary_struct(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        addr_row = result[result["column_name"] == "address"].iloc[0]
        assert addr_row["pattern_summary"] == "struct"

    def test_list_min_length(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        tags_row = result[result["column_name"] == "tags"].iloc[0]
        assert tags_row["min_length"] == 0  # empty list row

    def test_complex_min_max_null(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        addr_row = result[result["column_name"] == "address"].iloc[0]
        assert addr_row["min_val"] is None or str(addr_row["min_val"]) in ("None", "nan", "")

    def test_complex_numeric_stats_null(self, conn, complex_table_fixture):
        import math
        from anofox import profile
        result = profile.profile_table(conn, complex_table_fixture)
        tags_row = result[result["column_name"] == "tags"].iloc[0]
        assert tags_row["mean"] is None or (isinstance(tags_row["mean"], float) and math.isnan(tags_row["mean"]))


class TestProfileSummary:
    def test_returns_dataframe(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        import pandas as pd
        assert isinstance(result, pd.DataFrame)

    def test_one_row(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        assert len(result) == 1

    def test_row_column_count(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        assert result["row_count"].iloc[0] == 5
        assert result["column_count"].iloc[0] == 7

    def test_has_complex_columns_field(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        assert "complex_columns" in result.columns
        assert result["complex_columns"].iloc[0] == 0  # all scalars

    def test_has_total_nulls_field(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        assert "total_nulls" in result.columns
        assert result["total_nulls"].iloc[0] == 6  # row3:email + row4:name,age,salary,active,created_at

    def test_total_null_rate(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, profile_table_fixture)
        rate = result["total_null_rate"].iloc[0]
        assert abs(rate - 6.0 / 35) < 0.001

    def test_complex_columns_count(self, conn, complex_table_fixture):
        from anofox import profile
        result = profile.profile_summary(conn, complex_table_fixture)
        # _complex_test: tags (LIST), metadata (MAP), address (STRUCT) = 3 complex + id (INTEGER)
        assert result["complex_columns"].iloc[0] == 3

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import profile
        df = pd.DataFrame({"a": [1, 2, 3], "b": [4, 5, 6]})
        result = profile.profile_summary(conn, df)
        assert isinstance(result, pd.DataFrame)
        assert result["row_count"].iloc[0] == 3


class TestProfileCorrelations:
    def test_returns_dataframe(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_correlations(conn, profile_table_fixture)
        import pandas as pd
        assert isinstance(result, pd.DataFrame)

    def test_three_pairs(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_correlations(conn, profile_table_fixture)
        # id, age, salary → C(3,2) = 3 pairs
        assert len(result) == 3

    def test_pearson_spearman_columns(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_correlations(conn, profile_table_fixture)
        assert "pearson" in result.columns
        assert "spearman" in result.columns

    def test_pearson_in_range(self, conn, profile_table_fixture):
        from anofox import profile
        import math
        result = profile.profile_correlations(conn, profile_table_fixture)
        for v in result["pearson"].dropna():
            assert abs(v) <= 1.0 + 1e-9

    def test_spearman_in_range(self, conn, profile_table_fixture):
        from anofox import profile
        import math
        result = profile.profile_correlations(conn, profile_table_fixture)
        for v in result["spearman"].dropna():
            assert abs(v) <= 1.0 + 1e-9

    def test_columns_filter(self, conn, profile_table_fixture):
        from anofox import profile
        result = profile.profile_correlations(conn, profile_table_fixture, columns=["age", "salary"])
        assert len(result) == 1
        assert result["column_a"].iloc[0] == "age"
        assert result["column_b"].iloc[0] == "salary"
