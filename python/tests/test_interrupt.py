"""
Interrupting long extension calls (Ctrl-C in the CLI, ``Connection.interrupt()`` from Python).

The anomaly and PII table scans do their work inside a single DuckDB callback (a whole column,
a whole model fit), where DuckDB itself never checks for an interrupt, and the NER model takes
tens of milliseconds per value. Each test starts a call that runs for minutes when it is not
interrupted, interrupts it after one second and requires it to stop within a few seconds with
``duckdb.InterruptException``; the connection stays usable afterwards.

The NER tests need the NER model (OpenVINO build, model downloaded): set OPENVINO_AVAILABLE=1,
as for the NER SQL tests.
"""

import os
import threading
import time

import duckdb
import pytest

INTERRUPT_AFTER_S = 1.0
# Stopping takes milliseconds; the budget leaves room for slow CI machines
STOP_BUDGET_S = 10.0


@pytest.fixture
def raw_conn(ext_path):
    if ext_path is None:
        pytest.skip("Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'.")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext_path}'")
    con.execute("SET anofox_tab_trace_level = 'warn'")
    yield con
    try:
        con.execute("RESET anofox_pii_enabled_types")
        con.close()
    except duckdb.Error:
        pass  # still busy with a call that ignored the interrupt (the test has failed)


def _assert_stops_on_interrupt(con, sql):
    outcome = {}

    def run():
        try:
            con.execute(sql).fetchall()
            outcome["result"] = "finished"
        except BaseException as exc:  # noqa: BLE001 - the exception is the result
            outcome["result"] = exc

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    time.sleep(INTERRUPT_AFTER_S)
    assert worker.is_alive(), f"the call finished before it was interrupted: {sql}"
    interrupted_at = time.monotonic()
    con.interrupt()
    worker.join(STOP_BUDGET_S)
    stop_s = time.monotonic() - interrupted_at
    assert not worker.is_alive(), f"still running {STOP_BUDGET_S}s after the interrupt: {sql}"
    assert isinstance(outcome["result"], duckdb.InterruptException), outcome["result"]
    assert stop_s < STOP_BUDGET_S
    # The connection is usable again
    assert con.execute("SELECT 42").fetchone() == (42,)


def test_pattern_pii_scan_stops(raw_conn):
    # Three million support-ticket notes, pattern types only: about 90 s unless interrupted
    raw_conn.execute(
        "CREATE TABLE ticket_notes AS SELECT i AS ticket_id, "
        "'Customer asked for a callback at +49 30 ' || (1000000 + i) || ' or via mail to kunde' || i || "
        "'@example.de' AS note FROM range(3000000) t(i)"
    )
    raw_conn.execute("SET anofox_pii_enabled_types = 'EMAIL,PHONE'")
    _assert_stops_on_interrupt(raw_conn, "SELECT * FROM pii_scan_table('ticket_notes')")


def test_dbscan_mv_stops(raw_conn):
    # 200k two-dimensional points: the all-pairs neighbour search runs for many minutes
    raw_conn.execute(
        "CREATE TABLE store_sensors AS SELECT i AS reading_id, (i % 997) * 1.1 AS temperature_c, "
        "(i % 991) * 0.7 AS humidity_pct FROM range(200000) t(i)"
    )
    _assert_stops_on_interrupt(
        raw_conn,
        "SELECT count(*) FROM dbscan_mv('store_sensors', 'temperature_c, humidity_pct', 0.5, 5, 'clusters')",
    )


def test_isolation_forest_mv_stops(raw_conn):
    # Two million points scored by 500 trees
    raw_conn.execute(
        "CREATE TABLE store_sensors_year AS SELECT i AS reading_id, (i % 997) * 1.1 AS temperature_c, "
        "(i % 991) * 0.7 AS humidity_pct FROM range(2000000) t(i)"
    )
    _assert_stops_on_interrupt(
        raw_conn,
        "SELECT count(*) FROM isolation_forest_mv('store_sensors_year', 'temperature_c, humidity_pct', "
        "500, 4096, 0.02, 'scores', 42)",
    )


@pytest.fixture
def ner_conn(raw_conn):
    if not os.environ.get("OPENVINO_AVAILABLE"):
        pytest.skip("NER tests need OPENVINO_AVAILABLE=1 (OpenVINO build with the NER model)")
    raw_conn.execute("SELECT pii_detect('Anna Schmidt from Berlin called')").fetchall()
    status = raw_conn.execute("SELECT model_status FROM anofox_ner_status()").fetchone()[0]
    if status != "LOADED":
        pytest.skip(f"NER model not loaded (status {status})")
    # 20k distinct ticket texts with names, cities and companies: minutes of NER inference
    raw_conn.execute(
        "CREATE TABLE support_tickets AS SELECT i AS ticket_id, "
        "'Customer ' || (['Anna Schmidt', 'John Miller', 'Maria Rossi', 'Peter Wagner'])[1 + i % 4] || "
        "' from ' || (['Berlin', 'Munich', 'Hamburg', 'Vienna'])[1 + i % 4] || "
        "' wrote about order ' || i || ' placed with Acme GmbH' AS body FROM range(20000) t(i)"
    )
    return raw_conn


def test_ner_pii_scan_stops(ner_conn):
    _assert_stops_on_interrupt(ner_conn, "SELECT * FROM pii_scan_table('support_tickets')")


def test_ner_pii_audit_stops(ner_conn):
    _assert_stops_on_interrupt(ner_conn, "SELECT count(*) FROM pii_audit_table('support_tickets')")


def test_ner_pii_mask_stops(ner_conn):
    _assert_stops_on_interrupt(ner_conn, "SELECT count(pii_mask(body, 'redact')) FROM support_tickets")
