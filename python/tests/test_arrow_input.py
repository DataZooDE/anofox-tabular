"""
A pyarrow Table is accepted wherever a table name or DataFrame is (the table
functions); DuckDB copies it natively. The result is a pandas DataFrame, as for
a table name. The row-wise helpers need a DataFrame and say how to convert.
"""

import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")


@pytest.fixture
def invoices():
    amounts = [120.0 + (i % 6) * 4.5 for i in range(30)]
    amounts[17] = 4800.0      # a typo: 48.00 entered as 4800
    amounts[5] = math.nan     # not booked yet
    return pa.table({
        "invoice_no": [f"INV-{2026_0001 + i}" for i in range(30)],
        "customer": [["Brenner GmbH", "Kroll AG", "Meier & Co"][i % 3] for i in range(30)],
        "amount": amounts,
        "email": ["ap@brenner.de" if i % 3 == 0 else ("bad-address" if i == 4 else "ap@kroll.at")
                  for i in range(30)],
    })


def test_quality_check(conn, invoices):
    from anofox import quality
    assert quality.volume(conn, invoices, min_rows=30)["status"] == "pass"
    assert quality.null_rate(conn, invoices, "amount", max_null_rate=0.0)["null_count"] == 0


def test_profile(conn, invoices):
    from anofox import profile
    out = profile.profile_table(conn, invoices, columns=["customer"])
    assert isinstance(out, pd.DataFrame)
    assert out["distinct_count"].tolist() == [3]
    assert profile.profile_summary(conn, invoices)["row_count"].iloc[0] == 30


def test_anomaly_scores_and_nan(conn, invoices):
    from anofox import anomaly
    out = anomaly.isolation_forest(conn, invoices, "amount", output_mode="scores",
                                   id_column="invoice_no", seed=7, contamination=0.05)
    assert isinstance(out, pd.DataFrame)
    assert len(out) == 29  # the NaN amount is left out, as for pandas and polars
    assert out.loc[out["is_anomaly"], "invoice_no"].tolist() == ["INV-20260018"]
    summary = anomaly.dbscan(conn, invoices, "amount", eps=10.0, min_pts=3)
    assert (summary["total_count"], summary["noise_count"]) == (29, 1)


def test_diff_with_pandas(conn, invoices):
    from anofox import diff
    after = invoices.to_pandas()
    after.loc[17, "amount"] = 48.0
    changes = diff.joindiff(conn, invoices, after, primary_keys="invoice_no",
                            compare_columns=["amount"])
    assert changes["invoice_no"].tolist() == ["INV-20260018", "INV-20260006"] or \
        sorted(changes["invoice_no"].tolist()) == ["INV-20260006", "INV-20260018"]


def test_pii_scan(conn, invoices):
    from anofox import pii
    out = pii.pii_scan_table(conn, invoices, columns="email", types=["EMAIL"])
    assert isinstance(out, pd.DataFrame)
    assert len(out) > 0


def test_connection_methods(conn, invoices):
    out = conn.profile(invoices)
    assert isinstance(out, pd.DataFrame)
    assert out.set_index("column").loc["amount", "row_count"] == 30
    checks = conn.quality_check(invoices, {"volume": {"min_rows": 30},
                                           "distinct_count": {"column": "customer", "min_distinct": 3}})
    assert checks.status == "pass"


def test_row_wise_helper_says_how_to_convert(conn, invoices):
    from anofox import validate
    with pytest.raises(TypeError, match=r"pyarrow Table.*to_pandas\(\)"):
        validate.email_is_valid(conn, invoices, column="email")


def test_other_objects_list_the_supported_inputs(conn):
    from anofox import quality
    with pytest.raises(TypeError, match=r"pandas or polars\).*pyarrow Table"):
        quality.volume(conn, {"invoice_no": ["INV-1"]}, min_rows=1)
