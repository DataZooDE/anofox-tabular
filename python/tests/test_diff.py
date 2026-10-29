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

    def test_compound_primary_key(self, conn, diff_tables):
        import pandas as pd
        from anofox import diff
        src, tgt = diff_tables
        result = diff.hashdiff(conn, src, tgt, primary_keys=["id", "name"])
        assert isinstance(result, pd.DataFrame)
        assert len(result) > 0

    def test_bisection_threshold_rejected(self, conn, diff_tables):
        from anofox import diff
        src, tgt = diff_tables
        # The bisection algorithm is not implemented; the extension rejects
        # the parameters with a clear binder error instead of ignoring them.
        with pytest.raises(Exception, match="bisection"):
            diff.hashdiff(conn, src, tgt, primary_keys="id", bisection_threshold=1000)

    def test_bisection_factor_rejected(self, conn, diff_tables):
        from anofox import diff
        src, tgt = diff_tables
        with pytest.raises(Exception, match="bisection"):
            diff.hashdiff(
                conn, src, tgt, primary_keys="id",
                bisection_threshold=1000, bisection_factor=5,
            )


@pytest.fixture
def invoice_tables(conn):
    """A billing migration: renamed, dropped, added, retyped and re-cased columns."""
    conn.execute(
        "CREATE OR REPLACE TABLE _invoices_v1 (invoice_id INTEGER, customer_name VARCHAR, "
        "Amount DECIMAL(12, 2), currency VARCHAR, cost_center INTEGER, fax VARCHAR, issued_on DATE)"
    )
    conn.execute(
        "CREATE OR REPLACE TABLE _invoices_v2 (invoice_id INTEGER, customer_name VARCHAR, "
        "amount DECIMAL(12, 2), ccy VARCHAR, cost_center VARCHAR, issued_on DATE, payment_terms VARCHAR)"
    )
    yield "_invoices_v1", "_invoices_v2"
    conn.execute("DROP TABLE IF EXISTS _invoices_v1")
    conn.execute("DROP TABLE IF EXISTS _invoices_v2")


class TestDiffSchema:
    def test_table_names(self, conn, invoice_tables):
        import pandas as pd
        from anofox import diff
        result = diff.diff_schema(conn, *invoice_tables)
        assert isinstance(result, pd.DataFrame)
        assert list(result.columns) == ["column_name", "source_type", "target_type", "drift"]
        rows = [
            (r.column_name, r.source_type, r.target_type, r.drift)
            for r in result.astype(object).where(result.notna(), None).itertuples()
        ]
        assert rows == [
            ("invoice_id", "INTEGER", "INTEGER", "same"),
            ("customer_name", "VARCHAR", "VARCHAR", "same"),
            ("Amount", "DECIMAL(12,2)", "DECIMAL(12,2)", "same"),
            ("currency", "VARCHAR", None, "removed"),
            ("cost_center", "INTEGER", "VARCHAR", "retyped"),
            ("fax", "VARCHAR", None, "removed"),
            ("issued_on", "DATE", "DATE", "same"),
            ("ccy", None, "VARCHAR", "added"),
            ("payment_terms", None, "VARCHAR", "added"),
        ]

    def test_pandas_dataframes(self, conn):
        import pandas as pd
        from anofox import diff
        old = pd.DataFrame({"order_id": [1001, 1002], "total": [19.99, 5.5], "fax": ["n/a", "n/a"]})
        new = pd.DataFrame({"order_id": ["A-1001", "A-1002"], "total": [19.99, 5.5], "channel": ["web", "shop"]})
        result = diff.diff_schema(conn, old, new)
        assert isinstance(result, pd.DataFrame)
        assert dict(zip(result["column_name"], result["drift"])) == {
            "order_id": "retyped",
            "total": "same",
            "fax": "removed",
            "channel": "added",
        }

    def test_mixed_table_and_dataframe(self, conn, invoice_tables):
        import pandas as pd
        from anofox import diff
        new = pd.DataFrame({"invoice_id": [1], "customer_name": ["Müller GmbH"]})
        result = diff.diff_schema(conn, invoice_tables[0], new)
        drift = dict(zip(result["column_name"], result["drift"]))
        assert drift["invoice_id"] == "retyped"  # INTEGER vs the DataFrame's BIGINT
        assert drift["customer_name"] == "same"
        assert drift["fax"] == "removed"

    def test_polars_returns_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import diff
        old = pl.DataFrame({"sku": ["X-1"], "qty": [3]})
        new = pl.DataFrame({"sku": ["X-1"], "qty": [3.0]})
        result = diff.diff_schema(conn, old, new)
        assert isinstance(result, pl.DataFrame)
        assert result["drift"].to_list() == ["same", "retyped"]

    def test_unknown_table_raises(self, conn):
        import duckdb
        from anofox import diff
        with pytest.raises(duckdb.Error):
            diff.diff_schema(conn, "_no_such_table_v1", "_no_such_table_v2")

    def test_registered_tables_are_dropped(self, conn):
        import pandas as pd
        from anofox import diff
        before = conn.execute("SELECT count(*) FROM duckdb_tables()").fetchall()
        diff.diff_schema(conn, pd.DataFrame({"a": [1]}), pd.DataFrame({"a": [1]}))
        assert conn.execute("SELECT count(*) FROM duckdb_tables()").fetchall() == before


class TestTypeChangeWarningOnce:
    """The extension's type-change warning is printed once per call, not once per bind."""

    @pytest.mark.parametrize("func", ["joindiff", "hashdiff"])
    def test_warning_printed_once(self, conn, capfd, func):
        import pandas as pd
        from anofox import diff
        before = pd.DataFrame({"sku": ["A-100", "B-200"], "unit_price": [19.99, 4.5]})
        after = pd.DataFrame({"sku": ["A-100", "B-200"], "unit_price": ["19.99", "4.95"]})
        capfd.readouterr()
        result = getattr(diff, func)(conn, before, after, "sku")
        err = capfd.readouterr().err
        assert err.count("changed type DOUBLE -> VARCHAR") == 1
        assert result["sku"].tolist() == ["B-200"]
