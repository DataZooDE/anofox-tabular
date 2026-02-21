"""
Tests for _cli.py — argument parsing and CLI behaviour.
"""

import csv
import sys
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_csv(tmp_path: Path, rows: list[dict]) -> str:
    """Write a simple CSV file and return its path."""
    if not rows:
        raise ValueError("Need at least one row")
    path = tmp_path / "sample.csv"
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return str(path)


def _run_cli(*argv: str) -> tuple[int, str]:
    """
    Call _cli.main() with patched sys.argv and a captured rich Console.

    Returns (exit_code, stdout_text).
    Catches SystemExit so the caller can assert on the exit code.
    Non-integer exit codes (e.g. error strings) map to exit code 1.
    """
    from anofox._cli import main
    from rich.console import Console

    buf = StringIO()
    console = Console(file=buf, force_terminal=False, no_color=True)
    with patch("sys.argv", ["anofox", *argv]):
        try:
            main(console=console)
            code = 0
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else 1
    return code, buf.getvalue()


# ---------------------------------------------------------------------------
# Tests that do NOT require the extension
# ---------------------------------------------------------------------------

class TestReadFn:
    def test_parquet_returns_read_parquet(self):
        from anofox._cli import _read_fn
        fn = _read_fn("data.parquet")
        assert fn.startswith("read_parquet(")

    def test_csv_returns_read_csv(self):
        from anofox._cli import _read_fn
        fn = _read_fn("data.csv")
        assert fn.startswith("read_csv(")

    def test_tsv_includes_tab_delimiter(self):
        from anofox._cli import _read_fn
        fn = _read_fn("data.tsv")
        assert "read_csv(" in fn
        assert "\\t" in fn

    def test_json_returns_read_json(self):
        from anofox._cli import _read_fn
        fn = _read_fn("data.json")
        assert fn.startswith("read_json(")

    def test_ndjson_returns_read_json(self):
        from anofox._cli import _read_fn
        fn = _read_fn("data.ndjson")
        assert fn.startswith("read_json(")

    def test_unknown_extension_raises_system_exit(self):
        from anofox._cli import _read_fn
        with pytest.raises(SystemExit) as exc_info:
            _read_fn("file.xlsx")
        assert "Unsupported" in str(exc_info.value)

    def test_error_message_lists_supported_formats(self):
        from anofox._cli import _read_fn
        with pytest.raises(SystemExit) as exc_info:
            _read_fn("file.unknown")
        msg = str(exc_info.value)
        assert ".parquet" in msg or "parquet" in msg


class TestQualityArgValidation:
    def test_null_max_without_column_exits_nonzero(self, tmp_path):
        """--null-max requires --column; validation runs before connecting."""
        rows = [{"a": "1"}, {"a": "2"}]
        csv_path = _make_csv(tmp_path, rows)
        # Validation raises SystemExit before any connection is attempted
        code, _ = _run_cli("quality", csv_path, "--null-max", "0.5")
        assert code != 0

    def test_no_rules_exits_nonzero(self, tmp_path):
        """No quality rules should exit with a non-zero code."""
        rows = [{"a": "1"}]
        csv_path = _make_csv(tmp_path, rows)
        code, _ = _run_cli("quality", csv_path)
        assert code != 0


# ---------------------------------------------------------------------------
# Tests that DO require the extension
# ---------------------------------------------------------------------------

class TestProfileCommand:
    def test_profile_produces_output(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"id": i, "name": f"row{i}"} for i in range(10)]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli("profile", csv_path, "--ext", ext_path)
        assert code == 0
        assert "id" in out or "name" in out

    def test_profile_output_contains_null_rate(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"score": i} for i in range(5)]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli("profile", csv_path, "--ext", ext_path)
        assert code == 0
        assert "null_rate" in out

    def test_profile_json_format(self, ext_path, tmp_path):
        import json as _json
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"x": 1}, {"x": 2}]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli("profile", csv_path, "--ext", ext_path, "--format", "json")
        assert code == 0
        parsed = _json.loads(out)
        assert isinstance(parsed, list)
        assert any(r.get("column") == "x" for r in parsed)

    def test_profile_csv_format(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"a": "hello"}]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli("profile", csv_path, "--ext", ext_path, "--format", "csv")
        assert code == 0
        assert "column" in out  # CSV header


class TestQualityCommand:
    def test_quality_exits_0_on_pass(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"val": i} for i in range(20)]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli(
            "quality", csv_path, "--ext", ext_path, "--volume-min", "10"
        )
        assert code == 0
        assert "pass" in out

    def test_quality_exits_1_on_fail(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"val": i} for i in range(5)]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli(
            "quality", csv_path, "--ext", ext_path, "--volume-min", "1000"
        )
        assert code == 1
        assert "fail" in out

    def test_quality_json_format(self, ext_path, tmp_path):
        import json as _json
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"v": i} for i in range(10)]
        csv_path = _make_csv(tmp_path, rows)
        code, out = _run_cli(
            "quality", csv_path, "--ext", ext_path,
            "--volume-min", "5", "--format", "json",
        )
        assert code == 0
        parsed = _json.loads(out)
        assert "status" in parsed
        assert "checks" in parsed

    def test_null_rate_check_passes(self, ext_path, tmp_path):
        if ext_path is None:
            pytest.skip("No extension binary")
        rows = [{"score": i} for i in range(10)]
        csv_path = _make_csv(tmp_path, rows)
        code, _ = _run_cli(
            "quality", csv_path, "--ext", ext_path,
            "--null-max", "0.5", "--column", "score",
        )
        assert code == 0
