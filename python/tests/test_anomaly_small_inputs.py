"""
Small and empty inputs of the anomaly detectors.

An Isolation Forest needs some rows to tell an unusual value from a typical
one: with fewer than anomaly.MIN_ISOLATION_FOREST_ROWS scored rows the wrapper
warns. A per-row output without rows has the same columns and dtypes as one
with rows, so code like ``out["conditions"].apply(len)`` works on both.
"""

import sys
import warnings
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")


def _invoices(n):
    """n invoices of a small shop; the last one is far larger than the others.

    No invoice (n = 0) is the result of a filter that matched nothing, with the
    column dtypes of the full table.
    """
    if n == 0:
        # An explicit string dtype: an empty pandas 2 object column carries no
        # type, and DuckDB 1.5 copies it as INTEGER.
        return _invoices(3).astype({"invoice_id": "string"}).iloc[0:0]
    amounts = [80.0 + (i % 7) * 4.5 for i in range(n)]
    if n > 2:
        amounts[-1] = 2400.0
    return pd.DataFrame({
        "invoice_id": [f"INV-{2026_0001 + i}" for i in range(n)],
        "amount": amounts,
        "line_items": [float(1 + i % 4) for i in range(n)],
    })


class TestIsolationForestWarnsOnFewRows:
    @pytest.mark.parametrize("output_mode", ["summary", "scores"])
    def test_two_rows_warn(self, conn, output_mode):
        from anofox import anomaly
        with pytest.warns(UserWarning, match=r"isolation_forest: only 2 rows were scored.*at least 10"):
            anomaly.isolation_forest(conn, _invoices(2), "amount", output_mode=output_mode, seed=3)

    def test_warning_says_what_to_do(self, conn):
        from anofox import anomaly
        with pytest.warns(UserWarning) as record:
            anomaly.isolation_forest(conn, _invoices(4), "amount", seed=3)
        message = str(record[0].message)
        assert "Pass at least 10 rows" in message
        assert "fixed range or a business rule" in message
        assert "anomaly.MIN_ISOLATION_FOREST_ROWS" in message

    def test_multivariate_warns(self, conn):
        from anofox import anomaly
        with pytest.warns(UserWarning, match=r"isolation_forest_mv: only 5 rows were scored"):
            anomaly.isolation_forest_mv(conn, _invoices(5), ["amount", "line_items"], seed=3)

    def test_rows_left_out_do_not_count(self, conn):
        from anofox import anomaly
        invoices = _invoices(12)
        invoices.loc[0:2, "amount"] = None  # 9 rows scored
        with pytest.warns(UserWarning, match="only 9 rows"):
            anomaly.isolation_forest(conn, invoices, "amount", seed=3)

    @pytest.mark.parametrize("rows", [0, 10, 40])
    def test_no_warning(self, conn, rows):
        from anofox import anomaly
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            result = anomaly.isolation_forest(conn, _invoices(rows), "amount", seed=3)
        assert result["total_count"] == rows

    def test_the_minimum_is_public(self):
        from anofox import anomaly
        assert anomaly.MIN_ISOLATION_FOREST_ROWS == 10


PER_ROW = [
    ("isolation_forest", "amount", "scores", {"seed": 3}),
    ("isolation_forest_mv", ["amount", "line_items"], "scores", {"seed": 3}),
    ("dbscan", "amount", "clusters", {"eps": 5.0, "min_pts": 3}),
    ("dbscan_mv", ["amount", "line_items"], "clusters", {"eps": 5.0, "min_pts": 3}),
    ("outlier_tree", ["line_items", "amount"], "outliers", {"min_size_numeric": 5, "z_outlier": 3.0}),
]


def _run(conn, function, columns, mode, kwargs, df):
    from anofox import anomaly
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        return getattr(anomaly, function)(conn, df, columns, output_mode=mode,
                                          id_column="invoice_id", **kwargs)


@pytest.mark.parametrize("function,columns,mode,kwargs", PER_ROW, ids=[p[0] for p in PER_ROW])
class TestEmptyPerRowOutput:
    def test_pandas_dtypes_are_stable(self, conn, function, columns, mode, kwargs):
        full = _run(conn, function, columns, mode, kwargs, _invoices(60))
        empty = _run(conn, function, columns, mode, kwargs, _invoices(0))
        assert len(full) > 0 and len(empty) == 0
        assert empty.dtypes.to_dict() == full.dtypes.to_dict()

    def test_polars_schema_is_stable(self, conn, function, columns, mode, kwargs):
        full = _run(conn, function, columns, mode, kwargs, pl.from_pandas(_invoices(60)))
        empty = _run(conn, function, columns, mode, kwargs, pl.from_pandas(_invoices(0)))
        assert len(full) > 0 and len(empty) == 0
        assert empty.schema == full.schema


class TestOutlierTreeWithoutOutliers:
    """Data without an outlier gives an empty frame whose conditions column is still a list column."""

    @staticmethod
    def _calm():
        invoices = _invoices(60)
        invoices.loc[59, "amount"] = 95.0
        return invoices

    def test_pandas_conditions_column_is_object(self, conn):
        from anofox import anomaly
        out = anomaly.outlier_tree(conn, self._calm(), ["line_items", "amount"], output_mode="outliers")
        assert len(out) == 0
        assert out["conditions"].dtype == object
        assert out["conditions"].apply(len).tolist() == []

    def test_polars_conditions_column_is_a_list(self, conn):
        from anofox import anomaly
        out = anomaly.outlier_tree(conn, pl.from_pandas(self._calm()), ["line_items", "amount"],
                                   output_mode="outliers")
        assert len(out) == 0
        assert isinstance(out.schema["conditions"], pl.List)
