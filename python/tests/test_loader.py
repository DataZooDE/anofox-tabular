"""
Tests for _loader.py — no extension binary required.
"""

import hashlib
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from anofox._loader import (
    AnofoxLoader,
    _detect_duckdb_version,
    _detect_platform,
    _fetch_sha256,
    _verify_sha256,
)


# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------

class TestDetectPlatform:
    def test_linux_amd64(self):
        with patch("platform.system", return_value="Linux"), \
             patch("platform.machine", return_value="x86_64"):
            assert _detect_platform() == "linux_amd64"

    def test_linux_arm64(self):
        with patch("platform.system", return_value="Linux"), \
             patch("platform.machine", return_value="aarch64"):
            assert _detect_platform() == "linux_arm64"

    def test_macos_arm64(self):
        with patch("platform.system", return_value="Darwin"), \
             patch("platform.machine", return_value="arm64"):
            assert _detect_platform() == "osx_arm64"

    def test_macos_amd64(self):
        with patch("platform.system", return_value="Darwin"), \
             patch("platform.machine", return_value="x86_64"):
            assert _detect_platform() == "osx_amd64"

    def test_windows_amd64(self):
        with patch("platform.system", return_value="Windows"), \
             patch("platform.machine", return_value="amd64"):
            assert _detect_platform() == "windows_amd64"

    def test_returns_string(self):
        result = _detect_platform()
        assert isinstance(result, str)
        assert "_" in result


# ---------------------------------------------------------------------------
# DuckDB version detection
# ---------------------------------------------------------------------------

class TestDetectDuckdbVersion:
    def test_detects_version(self):
        with patch("importlib.metadata.version", return_value="1.1.3"):
            version = _detect_duckdb_version()
        assert version == "1.1.3"

    def test_raises_when_not_installed(self):
        import importlib.metadata
        with patch(
            "importlib.metadata.version",
            side_effect=importlib.metadata.PackageNotFoundError("duckdb"),
        ):
            with pytest.raises(RuntimeError, match="duckdb is not installed"):
                _detect_duckdb_version()

    def test_actual_version_is_string(self):
        version = _detect_duckdb_version()
        assert isinstance(version, str)
        parts = version.split(".")
        assert len(parts) >= 2


# ---------------------------------------------------------------------------
# AnofoxLoader
# ---------------------------------------------------------------------------

class TestAnofoxLoader:
    def test_default_extension_name(self):
        loader = AnofoxLoader()
        assert loader.extension_name == "anofox_tabular"

    def test_custom_extension_name(self):
        loader = AnofoxLoader(extension_name="anofox_forecast")
        assert loader.extension_name == "anofox_forecast"

    def test_duckdb_version_override(self):
        loader = AnofoxLoader(duckdb_version="1.0.0")
        assert loader.duckdb_version == "1.0.0"

    def test_cached_path_structure(self):
        loader = AnofoxLoader(duckdb_version="1.1.3")
        with patch("platform.system", return_value="Linux"), \
             patch("platform.machine", return_value="x86_64"):
            loader._arch = "linux_amd64"
        path = loader._cached_path()
        assert "anofox_tabular" in str(path)
        assert "1.1.3" in str(path)
        assert path.name == "anofox_tabular.duckdb_extension"

    def test_cached_path_is_under_home(self):
        loader = AnofoxLoader(duckdb_version="1.0.0")
        path = loader._cached_path()
        assert str(Path.home()) in str(path)

    def test_candidate_urls_include_s3(self):
        loader = AnofoxLoader(
            extension_name="anofox_tabular",
            s3_base_url="https://get.erpl.io",
            duckdb_version="1.1.3",
        )
        loader._arch = "linux_amd64"
        urls = loader._candidate_urls()
        assert any("get.erpl.io" in u for u in urls)
        assert any("anofox_tabular.duckdb_extension.gz" in u for u in urls)

    def test_candidate_urls_include_community_registry(self):
        loader = AnofoxLoader(duckdb_version="1.1.3")
        loader._arch = "linux_amd64"
        urls = loader._candidate_urls()
        assert any("community-extensions.duckdb.org" in u for u in urls)

    def test_ensure_extension_uses_local_path_when_provided(self, tmp_path):
        ext_file = tmp_path / "anofox_tabular.duckdb_extension"
        ext_file.write_bytes(b"fake")
        loader = AnofoxLoader()
        result = loader.ensure_extension(local_path=str(ext_file))
        assert result == str(ext_file)

    def test_ensure_extension_uses_cached_when_present(self, tmp_path):
        with patch("pathlib.Path.home", return_value=tmp_path):
            loader = AnofoxLoader(duckdb_version="9.9.9")
            # Manually place a file at the cached path
            cached = loader._cached_path()
            checksum = loader._cached_checksum_path()
            cached.parent.mkdir(parents=True, exist_ok=True)
            payload = b"fake cached"
            cached.write_bytes(payload)
            checksum.write_text(hashlib.sha256(payload).hexdigest())
            try:
                result = loader.ensure_extension()
                assert result == str(cached)
            finally:
                cached.unlink(missing_ok=True)
                checksum.unlink(missing_ok=True)

    def test_ensure_extension_raises_when_nothing_available(self):
        loader = AnofoxLoader(duckdb_version="0.0.0-nonexistent")
        loader._arch = "nonexistent_arch"
        with pytest.raises(RuntimeError, match="Could not find or download"):
            loader.ensure_extension()

    def test_custom_s3_base_url(self):
        loader = AnofoxLoader(
            s3_base_url="https://custom.example.com/",
            duckdb_version="1.0.0",
        )
        loader._arch = "linux_amd64"
        urls = loader._candidate_urls()
        assert any("custom.example.com" in u for u in urls)

    def test_forecast_extension_urls(self):
        """Demonstrate reusability for sibling extensions."""
        loader = AnofoxLoader(
            extension_name="anofox_forecast",
            s3_base_url="https://get.erpl.io",
            duckdb_version="1.1.3",
        )
        loader._arch = "linux_amd64"
        urls = loader._candidate_urls()
        assert any("anofox_forecast" in u for u in urls)


class TestIntegrityHelpers:
    def test_fetch_sha256_parses_standard_sidecar_format(self):
        class _Resp:
            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, tb):
                return False

            def read(self):
                return (("d2" * 32) + "  file.duckdb_extension.gz\n").encode("utf-8")

        with patch("urllib.request.urlopen", return_value=_Resp()):
            digest = _fetch_sha256("https://example.com/ext.gz.sha256")
        assert digest == "d2" * 32

    def test_verify_sha256_accepts_matching_hash(self):
        payload = b"payload"
        expected = hashlib.sha256(payload).hexdigest()
        _verify_sha256(payload, expected)

    def test_verify_sha256_rejects_mismatch(self):
        with pytest.raises(RuntimeError, match="SHA256 mismatch"):
            _verify_sha256(b"payload", "0" * 64)
