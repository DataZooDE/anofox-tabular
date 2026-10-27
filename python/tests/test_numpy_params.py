"""
Plain DuckDB connections.

The wrappers accept a plain ``duckdb.DuckDBPyConnection`` with the
extension loaded, not only an :class:`~anofox.AnofoxConnection`.
"""

import sys
from decimal import Decimal
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

np = pytest.importorskip("numpy")
pd = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def orders():
    """Realistic order lines: 40 normal orders and one bulk order."""
    rng = np.random.default_rng(7)
    n = 40
    frame = pd.DataFrame({
        "order_id": [f"SO-2026-{1000 + i}" for i in range(n)],
        "customer": [f"CUST-{100 + i % 9}" for i in range(n)],
        "status": ["shipped", "delivered", "open", "cancelled"] * (n // 4),
        "qty": rng.integers(1, 6, n),
        "unit_price": np.round(rng.uniform(9.5, 49.9, n), 2),
        "amount_cents": rng.integers(950, 24950, n),
        "currency": ["EUR"] * n,
        "ordered_at": pd.date_range("2026-09-01 08:00", periods=n, freq="6h"),
    })
    bulk = pd.DataFrame({
        "order_id": ["SO-2026-2000"], "customer": ["CUST-999"], "status": ["open"],
        "qty": [480], "unit_price": [8.75], "amount_cents": [420000], "currency": ["EUR"],
        "ordered_at": [pd.Timestamp("2026-09-11 09:30")],
    })
    return pd.concat([frame, bulk], ignore_index=True)


@pytest.fixture(scope="module")
def plain_conn(ext_path):
    """A plain duckdb connection with the extension loaded (no AnofoxConnection)."""
    if ext_path is None:
        pytest.skip("Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'.")
    import duckdb
    connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    escaped = ext_path.replace("'", "''")
    connection.execute(f"LOAD '{escaped}'")
    yield connection
    connection.close()


# ---------------------------------------------------------------------------
# A plain duckdb connection, once per public wrapper module
# ---------------------------------------------------------------------------

class TestPlainConnection:
    def test_quality(self, plain_conn, orders):
        from anofox import quality
        assert quality.volume(plain_conn, orders, 1)["status"] == "pass"
        quality.define_checks(plain_conn, [
            {"check_name": "orders_volume", "check_type": "volume", "table_name": "plain_orders",
             "lower_threshold": 1},
        ], checks_table="plain_dq_checks")
        plain_conn.execute("CREATE OR REPLACE TABLE plain_orders AS SELECT 1 AS id")
        results = quality.run_checks(plain_conn, "plain_dq_checks")
        assert [r["status"] for r in results] == ["pass"]

    def test_anomaly(self, plain_conn, orders):
        from anofox import anomaly
        summary = anomaly.isolation_forest(plain_conn, orders, "qty", seed=7)
        assert summary["total_count"] == len(orders)
        rows = anomaly.dbscan(plain_conn, orders, "qty", output_mode="scores")
        assert len(rows) == len(orders)

    def test_profile(self, plain_conn, orders):
        from anofox import profile
        assert "qty" in profile.profile_table(plain_conn, orders)["column_name"].tolist()
        assert len(profile.profile_summary(plain_conn, orders)) == 1

    def test_diff(self, plain_conn, orders):
        from anofox import diff
        target = orders.copy()
        target.loc[0, "status"] = "returned"
        result = diff.joindiff(plain_conn, orders, target, "order_id")
        assert result["order_id"].tolist() == ["SO-2026-1000"]

    def test_money(self, plain_conn, orders):
        from anofox import money
        assert money.money_from_cents(plain_conn, 1050, "EUR") == {"amount": Decimal("10.50"), "currency": "EUR"}
        out = money.make_money(plain_conn, orders.head(2), "EUR", column="unit_price")
        assert len(out["make_money"]) == 2

    def test_validate(self, plain_conn):
        from anofox import validate
        contacts = pd.DataFrame({"email": ["anna.berger@example.com", "not-an-email"]})
        out = validate.email_is_valid(plain_conn, contacts, column="email")
        assert out["email_is_valid"].tolist() == [True, False]

    def test_vat(self, plain_conn):
        from anofox import vat
        suppliers = pd.DataFrame({"vat_id": ["DE123456789", "XX1"]})
        out = vat.vat_is_valid_syntax(plain_conn, suppliers, column="vat_id")
        assert out["vat_is_valid_syntax"].tolist() == [True, False]

    def test_pii(self, plain_conn):
        from anofox import pii
        assert pii.pii_mask(plain_conn, "Contact anna.berger@example.com today", types="EMAIL") == (
            "Contact [EMAIL] today"
        )

