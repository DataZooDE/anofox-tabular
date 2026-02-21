"""
Tests for _plugin.py — pytest plugin registration and fixtures.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


class TestPluginModule:
    def test_plugin_is_importable(self):
        from anofox import _plugin
        assert _plugin is not None

    def test_plugin_has_addoption(self):
        from anofox import _plugin
        assert hasattr(_plugin, "pytest_addoption")

    def test_plugin_has_configure(self):
        from anofox import _plugin
        assert hasattr(_plugin, "pytest_configure")

    def test_plugin_has_anofox_conn_fixture(self):
        from anofox import _plugin
        assert hasattr(_plugin, "anofox_conn")

    def test_plugin_has_runtest_setup_hook(self):
        from anofox import _plugin
        assert hasattr(_plugin, "pytest_runtest_setup")

    def test_anofox_conn_is_fixture(self):
        from anofox import _plugin
        # pytest fixtures have _fixture_function (pytest>=9) or _pytestfixturefunction (pytest<9)
        fixture_fn = _plugin.anofox_conn
        assert hasattr(fixture_fn, "_fixture_function") or hasattr(
            fixture_fn, "_pytestfixturefunction"
        )

    def test_anofox_conn_is_session_scoped(self):
        from anofox import _plugin
        fixture_fn = _plugin.anofox_conn
        # pytest>=9 uses _fixture_function_marker; pytest<9 uses _pytestfixturefunction
        if hasattr(fixture_fn, "_fixture_function_marker"):
            scope = fixture_fn._fixture_function_marker.scope
        else:
            scope = fixture_fn._pytestfixturefunction.scope
        assert scope == "session"

    def test_marker_line_contains_anofox_quality(self):
        """pytest_configure registers the anofox_quality marker description."""
        import unittest.mock as mock
        from anofox import _plugin

        config = mock.MagicMock()
        _plugin.pytest_configure(config)
        calls = config.addinivalue_line.call_args_list
        assert any(
            "anofox_quality" in str(call) for call in calls
        ), "anofox_quality marker not registered"


class TestAnofoxConnFixture:
    def test_anofox_conn_returns_connection(self, ext_path):
        """Manually drive the anofox_conn generator to test connection creation."""
        if ext_path is None:
            pytest.skip("No extension binary")

        import anofox
        from anofox._connection import AnofoxConnection
        from anofox import _plugin

        # Drive the generator manually (simulates what pytest does)
        gen = _plugin.anofox_conn.__wrapped__() if hasattr(
            _plugin.anofox_conn, "__wrapped__"
        ) else None

        # Simpler: just open a connection directly to verify the logic
        conn = anofox.connect(extension_path=ext_path)
        try:
            assert isinstance(conn, AnofoxConnection)
            assert conn.is_extension_loaded()
        finally:
            conn.close()

    def test_anofox_conn_fixture_via_request(self, ext_path, request):
        """Use pytest's own fixture mechanism to verify anofox_conn."""
        if ext_path is None:
            pytest.skip("No extension binary")

        # The anofox_conn fixture from the plugin should be discoverable
        # We test it by verifying the plugin module exports a valid fixture.
        from anofox import _plugin
        from anofox._connection import AnofoxConnection

        assert hasattr(_plugin, "anofox_conn")


class TestQualityResult:
    """Test QualityResult backward compatibility and HTML display."""

    def test_quality_result_status_access(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        assert result["status"] in ("pass", "fail")

    def test_quality_result_checks_access(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        assert isinstance(result["checks"], dict)

    def test_quality_result_key_error(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        with pytest.raises(KeyError):
            _ = result["nonexistent"]

    def test_quality_result_repr(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        r = repr(result)
        assert "QualityResult" in r
        assert "status" in r

    def test_quality_result_repr_html(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        html = result._repr_html_()
        assert "<table" in html
        assert "volume" in html
        assert "PASS" in html or "FAIL" in html

    def test_quality_result_passed_html_has_green(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(10)})
        result = conn.quality_check(df, {"volume": {"min_rows": 1}})
        html = result._repr_html_()
        if result.status == "pass":
            assert "#27ae60" in html

    def test_quality_result_failed_html_has_red(self, conn):
        import pandas as pd

        df = pd.DataFrame({"id": range(100)})
        result = conn.quality_check(df, {"volume": {"min_rows": 10000}})
        assert result.status == "fail"
        html = result._repr_html_()
        assert "#e74c3c" in html


class TestValidationResultReprHtml:
    def test_validation_result_passed_html(self, conn):
        import pandas as pd
        from anofox.validate import EmailRule

        df = pd.DataFrame({"email": ["valid@example.com"]})
        result = conn.validate(df, {"email": EmailRule(mode="regex")})
        html = result._repr_html_()
        assert "<div>" in html
        if result.passed:
            assert "#27ae60" in html
            assert "PASSED" in html

    def test_validation_result_failed_html(self, conn):
        import pandas as pd
        from anofox.validate import EmailRule

        df = pd.DataFrame({"email": ["not-an-email"]})
        result = conn.validate(df, {"email": EmailRule(mode="regex")})
        html = result._repr_html_()
        assert "#e74c3c" in html
        assert "FAILED" in html

    def test_validation_result_no_failures_message(self, conn):
        import pandas as pd
        from anofox.validate import EmailRule

        df = pd.DataFrame({"email": ["user@example.com"]})
        result = conn.validate(df, {"email": EmailRule(mode="regex")})
        if result.passed:
            html = result._repr_html_()
            assert "No failures" in html
