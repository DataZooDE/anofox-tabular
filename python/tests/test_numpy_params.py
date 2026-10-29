"""
Values taken from a DataFrame as parameters, and plain DuckDB connections.

A value read from a DataFrame (``df["qty"].max()``, ``df["amount"].iloc[0]``)
is a numpy scalar, which the DuckDB 2.0 client cannot bind on its own
("Unable to transform python value of type numpy.int64"). Every wrapper must
accept such values wherever it accepts a Python number, string or time.

The wrappers also accept a plain ``duckdb.DuckDBPyConnection`` with the
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
# The conversion itself
# ---------------------------------------------------------------------------

class TestBindValue:
    def test_numpy_scalars_become_python_values(self):
        import datetime
        from anofox._sql import bind_value
        assert type(bind_value(np.int64(7))) is int
        assert type(bind_value(np.uint8(7))) is int
        assert type(bind_value(np.float32(0.5))) is float
        assert bind_value(np.float32(0.5)) == 0.5
        assert bind_value(np.bool_(True)) is True
        assert type(bind_value(np.str_("EUR"))) is str
        assert bind_value(np.datetime64("2026-09-11T09:30:00")) == datetime.datetime(2026, 9, 11, 9, 30)
        assert bind_value(np.datetime64("2026-09-11")) == datetime.date(2026, 9, 11)
        assert bind_value(np.datetime64("NaT")) is None
        assert bind_value(np.timedelta64(90, "m")) == datetime.timedelta(minutes=90)
        assert bind_value(pd.Timestamp("2026-09-11 09:30")) == datetime.datetime(2026, 9, 11, 9, 30)
        assert type(bind_value(pd.Timestamp("2026-09-11 09:30"))) is datetime.datetime

    def test_containers_are_converted_element_by_element(self):
        from anofox._sql import bind_params, bind_value
        converted = bind_value([np.int64(1), (np.float64(2.5), np.str_("x"))])
        assert converted == [1, (2.5, "x")]
        assert type(converted[0]) is int
        assert bind_value(np.array(["open", "shipped"], dtype=object)) == ["open", "shipped"]
        assert bind_params({"n": np.int64(3)}) == {"n": 3}

    def test_other_values_are_unchanged(self):
        from anofox._sql import bind_value
        for value in (None, 3, 2.5, "EUR", Decimal("1.25"), True, b"x"):
            assert bind_value(value) is value


# ---------------------------------------------------------------------------
# numpy values from a DataFrame, per wrapper module
# ---------------------------------------------------------------------------

class TestNumpyParameters:
    def test_connection_execute(self, conn, orders):
        row = conn.execute("SELECT ? + 1, ?", [orders["qty"].max(), orders["unit_price"].min()]).fetchone()
        assert row == (481, float(orders["unit_price"].min()))

    def test_quality_checks(self, conn, orders):
        from anofox import quality
        assert isinstance(orders["qty"].min(), np.integer)
        result = quality.volume(conn, orders, min_rows=orders["qty"].min(), max_rows=np.int64(len(orders)))
        assert result["status"] == "pass"
        assert result["row_count"] == len(orders)
        assert quality.distinct_count(
            conn, orders, "customer", min_distinct=orders["qty"].min()
        )["status"] == "pass"
        assert quality.zscore(conn, orders, "qty", threshold=np.float32(3.0))["status"] == "fail"
        assert quality.values_in_set(
            conn, orders, "status", allowed_values=orders["status"].unique(), min_rate=np.float64(1.0)
        )["status"] == "pass"
        assert quality.duplicate_count(conn, orders, "order_id", max_duplicates=np.int64(0))["status"] == "pass"

    def test_quality_times_from_a_dataframe(self, conn, orders):
        from anofox import quality
        newest = orders["ordered_at"].max()  # pandas.Timestamp
        result = quality.freshness(conn, orders, "ordered_at", "1 day", reference_time=newest)
        assert result["status"] == "pass"
        result = quality.rel_count_change(
            conn, orders, "ordered_at", window_days=np.int64(3),
            reference_date=orders["ordered_at"].dt.date.max(),
        )
        assert result["status"] in ("pass", "fail")
        result = quality.rel_count_change(
            conn, orders, "ordered_at", window_days=np.int64(3),
            reference_date=orders["ordered_at"].values.max(),  # numpy.datetime64
        )
        assert result["status"] in ("pass", "fail")

    def test_anomaly(self, conn, orders):
        from anofox import anomaly
        result = anomaly.isolation_forest(
            conn, orders, "qty", n_trees=np.int64(50), sample_size=np.int64(32),
            contamination=np.float32(0.05), seed=np.int64(42),
        )
        assert result["total_count"] == len(orders)
        result = anomaly.dbscan(conn, orders, "qty", eps=np.float32(1.5), min_pts=np.int64(3))
        assert result["total_count"] == len(orders)
        result = anomaly.outlier_tree(conn, orders, ["qty", "unit_price"], max_depth=np.int64(2))
        assert isinstance(result, dict)

    def test_profile(self, conn, orders):
        from anofox import profile
        result = profile.profile_table(conn, orders, columns="qty", sample_size=np.int64(len(orders)))
        assert result["column_name"].tolist() == ["qty"]

    def test_money(self, conn, orders):
        from anofox import money
        cents = orders["amount_cents"].iloc[-1]
        assert isinstance(cents, np.integer)
        assert money.money_from_cents(conn, cents, orders["currency"].to_numpy(dtype=str)[0]) == {
            "amount": Decimal("4200.00"), "currency": "EUR",
        }
        assert money.make_money(conn, orders["unit_price"].iloc[-1], "EUR") == {
            "amount": Decimal("8.750"), "currency": "EUR",
        }
        assert money.make_money(conn, orders["qty"].iloc[-1], "EUR") == {
            "amount": Decimal("480.000"), "currency": "EUR",
        }
        assert money.make_money(conn, np.float32(12.5), "EUR") == {
            "amount": Decimal("12.500"), "currency": "EUR",
        }
        price = money.make_money(conn, Decimal("8.75"), "EUR")
        assert money.money_multiply(conn, price, orders["qty"].iloc[-1]) == {
            "amount": Decimal("4200.000"), "currency": "EUR",
        }

    def test_diff(self, conn, orders):
        from anofox import diff
        target = orders.copy()
        target.loc[3, "qty"] = 99
        target.loc[5, "status"] = "returned"
        result = diff.joindiff(
            conn, orders, target, orders.columns[:1].to_numpy(),
            compare_columns=np.array(["qty"]),
        )
        assert result["order_id"].tolist() == ["SO-2026-1003"]

    def test_check_suite(self, conn, orders):
        from anofox import quality
        conn.execute("CREATE OR REPLACE TABLE _np_orders AS SELECT * FROM range(41) t(id)")
        quality.define_checks(conn, [
            {"check_name": "orders_volume", "check_type": "volume", "table_name": "_np_orders",
             "lower_threshold": orders["qty"].min(), "upper_threshold": np.int64(len(orders))},
            {"check_name": "status_set", "check_type": "values_in_set", "table_name": "_np_orders",
             "column_name": "id", "params": {"allowed_values": np.arange(41).astype(str)},
             "lower_threshold": np.float32(1.0)},
        ], checks_table="_np_checks")
        try:
            results = quality.run_checks(conn, "_np_checks")
            assert {r["check_name"]: r["status"] for r in results} == {
                "orders_volume": "pass", "status_set": "pass",
            }
        finally:
            conn.execute("DROP TABLE IF EXISTS _np_orders")
            conn.execute("DROP TABLE IF EXISTS _np_checks")


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

    def test_numpy_parameters(self, plain_conn, orders):
        from anofox import quality
        assert quality.volume(plain_conn, orders, orders["qty"].min())["status"] == "pass"
