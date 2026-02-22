"""
Pytest plugin for anofox-tabular.

Registers:
- ``--anofox-check`` CLI flag: activates quality gate enforcement
- ``anofox_conn`` fixture: session-scoped AnofoxConnection (auto-skips if unavailable)
- ``@pytest.mark.anofox_quality(table, **rules)`` marker

Usage in ``conftest.py`` or test files::

    import pytest

    @pytest.mark.anofox_quality("my_table", volume_min=100)
    def test_table_has_enough_rows(anofox_conn):
        ...

Run with::

    pytest --anofox-check
"""

from __future__ import annotations

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--anofox-check",
        action="store_true",
        default=False,
        help="Enable anofox quality gate checks in tests",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers",
        "anofox_quality(table, **rules): run anofox quality checks before this test. "
        "Supported rules: volume_min=N, null_max=F (requires column=COL).",
    )


@pytest.fixture(scope="session")
def anofox_conn():
    """
    Session-scoped AnofoxConnection.

    Auto-skips when the anofox_tabular extension is unavailable.
    Extension path is resolved from ANOFOX_EXT_PATH environment variable
    or the default S3 download location.
    """
    try:
        import anofox

        conn = anofox.connect()
        yield conn
        conn.close()
    except Exception as exc:
        pytest.skip(f"anofox extension unavailable: {exc}")


def pytest_runtest_setup(item: pytest.Item) -> None:
    """
    Before each test, check for ``anofox_quality`` marker.

    Only executes the quality check when ``--anofox-check`` is active.
    """
    marker = item.get_closest_marker("anofox_quality")
    if marker is None:
        return
    if not item.config.getoption("--anofox-check", default=False):
        return

    conn = item._request.getfixturevalue("anofox_conn")  # type: ignore[attr-defined]

    table = marker.args[0] if marker.args else None
    kwargs = marker.kwargs

    if not table or not kwargs:
        return

    from anofox import quality as q

    results: dict = {}
    if "volume_min" in kwargs:
        results["volume"] = q.volume(conn, table, min_rows=kwargs["volume_min"])
    if "null_max" in kwargs:
        col = kwargs.get("column")
        if col:
            results["null_rate"] = q.null_rate(
                conn, table,
                column_name=col,
                max_null_rate=kwargs["null_max"],
            )

    failed = [name for name, r in results.items() if r.get("status") != "pass"]
    if failed:
        pytest.fail(
            f"anofox quality check failed for: {', '.join(failed)} "
            f"(table={table!r})"
        )
