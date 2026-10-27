"""
One statement, one configuration.

The PII and email options are process-wide: ``SET anofox_pii_enabled_types`` in one connection
changes them for every connection. A statement that is running while another connection changes
them must still use one configuration for all of its rows: before, the setting was read per data
chunk, and ``sum(pii_count(note))`` over 80,000 rows returned a mix such as 43,136 while a
second connection toggled the setting.

Each test runs the statement several times while a second thread toggles the setting in a loop
and requires every result to be one of the two values a single configuration gives.
"""

import threading

import duckdb
import pytest

ROWS = 80_000
RUNS = 8


@pytest.fixture
def db(ext_path):
    if ext_path is None:
        pytest.skip("Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'.")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext_path}'")
    con.execute("SET anofox_tab_trace_level = 'warn'")
    con.execute("SET threads = 4")
    # CRM notes: every row holds one e-mail address and one IBAN
    con.execute(
        f"""
        CREATE TABLE crm_notes AS
        SELECT i AS note_id,
               'Kunde ' || i || ' bittet um Rückruf, max.mustermann' || i || '@example.com, '
               || 'Erstattung auf DE89 3704 0044 0532 0130 00' AS note,
               'kunde' || i || '@example.com' AS email
        FROM range({ROWS}) t(i)
        """
    )
    yield con
    for setting in (
        "anofox_pii_enabled_types",
        "anofox_pii_default_mask_strategy",
        "anofox_tab_email_regex_pattern",
    ):
        con.execute(f"RESET {setting}")
    con.close()


def _results_while_toggling(con, query, toggles):
    """Runs `query` RUNS times while another connection cycles through `toggles`."""
    con.execute(toggles[0])
    stop = threading.Event()
    toggler = con.cursor()
    errors = []

    def toggle():
        try:
            while not stop.is_set():
                for statement in toggles:
                    toggler.execute(statement)
        except BaseException as exc:  # noqa: BLE001 - reported by the test
            errors.append(exc)

    worker = threading.Thread(target=toggle, daemon=True)
    worker.start()
    try:
        results = [con.cursor().execute(query).fetchone()[0] for _ in range(RUNS)]
    finally:
        stop.set()
        worker.join(30)
        toggler.close()
    assert not errors, errors
    return results


def test_pii_enabled_types_do_not_change_inside_a_statement(db):
    results = _results_while_toggling(
        db,
        "SELECT sum(pii_count(note)) FROM crm_notes",
        [
            "SET anofox_pii_enabled_types = 'EMAIL'",
            "SET anofox_pii_enabled_types = 'EMAIL,IBAN'",
        ],
    )
    assert set(results) <= {ROWS, 2 * ROWS}, results


def test_pii_mask_strategy_does_not_change_inside_a_statement(db):
    # Pattern types only: the NER model would make the runs slow
    db.execute("SET anofox_pii_enabled_types = 'EMAIL'")
    results = _results_while_toggling(
        db,
        "SELECT count(DISTINCT pii_redact_column(email)) FROM crm_notes",
        [
            "SET anofox_pii_default_mask_strategy = 'redact'",
            "SET anofox_pii_default_mask_strategy = 'asterisk'",
        ],
    )
    # redact gives '[EMAIL]' for every row; asterisk one '*' per character
    # (the addresses have a handful of different lengths)
    db.execute("SET anofox_pii_default_mask_strategy = 'asterisk'")
    asterisk = db.execute("SELECT count(DISTINCT pii_redact_column(email)) FROM crm_notes").fetchone()[0]
    assert set(results) <= {1, asterisk}, results


def test_pii_scan_table_uses_one_configuration(db):
    db.execute("CREATE TABLE notes AS SELECT note FROM crm_notes")
    results = _results_while_toggling(
        db,
        "SELECT sum(match_count) FROM pii_scan_table('notes')",
        [
            "SET anofox_pii_enabled_types = 'EMAIL'",
            "SET anofox_pii_enabled_types = 'EMAIL,IBAN'",
        ],
    )
    assert set(results) <= {ROWS, 2 * ROWS}, results


def test_email_regex_pattern_does_not_change_inside_a_statement(db):
    results = _results_while_toggling(
        db,
        "SELECT count(*) FILTER (WHERE email_is_valid(email, 'regex')) FROM crm_notes",
        [
            "SET anofox_tab_email_regex_pattern = '^nobody@nowhere\\.invalid$'",
            "RESET anofox_tab_email_regex_pattern",
        ],
    )
    assert set(results) <= {0, ROWS}, results
