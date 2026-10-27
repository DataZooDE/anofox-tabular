"""
Documented result shapes: the per-row anomaly column name, the email_validate
dict column and the return annotations of the public wrappers.
"""

import importlib
import inspect
import sys
import typing
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")


def _orders():
    totals = [42.5, 39.9, 45.0, 41.2, 44.8, 40.1, 43.3, 38.7, 46.2, 41.9, 39.5, 980.0]
    return pd.DataFrame({"order_id": [f"SO-{1001 + i}" for i in range(12)], "order_total": totals})


class TestUnivariateValueColumn:
    """SQL names the scored column `value`; the wrappers keep the SQL name (documented)."""

    def test_isolation_forest(self, conn):
        from anofox import anomaly
        out = anomaly.isolation_forest(conn, _orders(), "order_total", output_mode="scores",
                                       sample_size=12, id_column="order_id", seed=1)
        assert out.columns.tolist() == ["row_id", "order_id", "value", "anomaly_score", "is_anomaly"]
        assert out["value"].tolist() == _orders()["order_total"].tolist()

    def test_dbscan(self, conn):
        from anofox import anomaly
        out = anomaly.dbscan(conn, pl.from_pandas(_orders()), "order_total", output_mode="clusters",
                             epsilon=5.0, min_points=3)
        assert "value" in out.columns and "order_total" not in out.columns


class TestEmailValidateColumn:
    @staticmethod
    def _signups():
        return pd.DataFrame({
            "signup_id": [501, 502, 503],
            "email": ["jonas.weber@example.de", "info(at)kanzlei-roth.de", None],
        })

    def test_pandas_dicts_expand_to_columns(self, conn):
        from anofox import validate
        out = validate.email_validate(conn, self._signups(), column="email")
        checked = out["email_validate"].dropna()
        details = pd.DataFrame(checked.tolist(), index=checked.index)
        flat = out.join(details[["valid", "stage", "reason"]])
        assert flat["valid"].tolist()[:2] == [True, False]
        assert flat["reason"].iloc[1] == "invalid_format"
        assert flat["valid"].isna().tolist() == [False, False, True]

    def test_polars_struct_unnests(self, conn):
        from anofox import validate
        out = validate.email_validate(conn, pl.from_pandas(self._signups()), column="email")
        flat = out.unnest("email_validate")
        assert flat["valid"].to_list() == [True, False, None]
        assert {"stage", "reason", "mx_hosts", "smtp_debug"} <= set(flat.columns)


PUBLIC_MODULES = ["validate", "vat", "money", "pii", "anomaly", "diff", "profile", "quality"]


@pytest.mark.parametrize("module_name", PUBLIC_MODULES)
def test_public_wrappers_have_concrete_return_types(module_name):
    module = importlib.import_module(f"anofox.{module_name}")
    namespace = {**vars(module), "pandas": pd, "polars": pl}
    for name, function in inspect.getmembers(module, inspect.isfunction):
        if name.startswith("_") or function.__module__ != module.__name__:
            continue
        if name in ("enabled_types",):  # a context manager
            continue
        hints = typing.get_type_hints(function, globalns=namespace)
        assert "return" in hints, name
        assert hints["return"] is not typing.Any, name
        assert typing.Any not in typing.get_args(hints["return"]), name
