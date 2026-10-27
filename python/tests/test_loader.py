"""
Tests for _loader.py — no extension binary required.
"""

import gzip
import hashlib
import os
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from anofox._loader import (
    AnofoxLoader,
    UnsupportedDuckDBVersionError,
    _detect_duckdb_version,
    _detect_platform,
    _download,
    _duckdb_tag,
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

    def test_candidate_urls_follow_duckdb_repository_layout(self):
        """Community registry first, then the S3 mirror, both as <base>/<tag>/<platform>/<name>.gz."""
        loader = AnofoxLoader(
            extension_name="anofox_tabular",
            s3_base_url="https://get.erpl.io",
            duckdb_version="1.5.6",
        )
        loader._arch = "linux_amd64"
        assert loader._candidate_urls() == [
            "https://community-extensions.duckdb.org/v1.5.6/linux_amd64/anofox_tabular.duckdb_extension.gz",
            "https://get.erpl.io/v1.5.6/linux_amd64/anofox_tabular.duckdb_extension.gz",
        ]

    def test_candidate_urls_keep_an_existing_v_prefix(self):
        loader = AnofoxLoader(duckdb_version="v1.5.6")
        loader._arch = "osx_arm64"
        assert loader._candidate_urls() == [
            "https://community-extensions.duckdb.org/v1.5.6/osx_arm64/anofox_tabular.duckdb_extension.gz",
            "https://get.erpl.io/v1.5.6/osx_arm64/anofox_tabular.duckdb_extension.gz",
        ]

    def test_duckdb_tag_adds_v_prefix_once(self):
        assert _duckdb_tag("1.5.6") == "v1.5.6"
        assert _duckdb_tag("v1.5.6") == "v1.5.6"

    def test_duckdb_2_0_maps_to_its_deploy_path(self):
        """The DuckDB 2.0 package (``2.0.0``) downloads from ``v2.0.0/<platform>/``."""
        assert _duckdb_tag("2.0.0") == "v2.0.0"
        loader = AnofoxLoader(duckdb_version="2.0.0")
        loader._arch = "linux_amd64"
        assert loader._candidate_urls() == [
            "https://community-extensions.duckdb.org/v2.0.0/linux_amd64/anofox_tabular.duckdb_extension.gz",
            "https://get.erpl.io/v2.0.0/linux_amd64/anofox_tabular.duckdb_extension.gz",
        ]

    @pytest.mark.parametrize(
        "version",
        ["1.5.7.dev42", "2.0.0.dev1", "2.0.0rc1", "2.0.0a1", "2.0.0b2", "1.5.6.post1",
         "2.0.0.dev7+g1a2b3c", "2.0", "nightly"],
    )
    def test_duckdb_tag_rejects_versions_without_a_published_build(self, version):
        with pytest.raises(UnsupportedDuckDBVersionError) as excinfo:
            _duckdb_tag(version)
        message = str(excinfo.value)
        assert version in message
        assert "ANOFOX_EXT_PATH" in message

    @pytest.mark.parametrize("version", ["2.0.0.dev1", "2.0.0rc1", "1.5.6.post1"])
    def test_ensure_extension_rejects_prerelease_before_any_network_request(self, tmp_path, version):
        with patch("pathlib.Path.home", return_value=tmp_path):
            loader = AnofoxLoader(duckdb_version=version, allow_insecure_download=True)
            with patch("urllib.request.urlopen", side_effect=AssertionError("must not hit the network")):
                with pytest.raises(UnsupportedDuckDBVersionError, match="ANOFOX_EXT_PATH"):
                    loader.ensure_extension()

    def test_unsupported_version_error_is_a_runtime_error(self):
        """Callers catching the loader's RuntimeError keep working."""
        assert issubclass(UnsupportedDuckDBVersionError, RuntimeError)

    def test_prerelease_duckdb_still_loads_a_local_build(self, tmp_path):
        ext_file = tmp_path / "anofox_tabular.duckdb_extension"
        ext_file.write_bytes(b"fake")
        loader = AnofoxLoader(duckdb_version="2.0.0.dev1")
        assert loader.ensure_extension(local_path=str(ext_file)) == str(ext_file)

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
        loader = AnofoxLoader(duckdb_version="0.0.0")
        loader._arch = "nonexistent_arch"
        with pytest.raises(RuntimeError, match="Could not find or download"):
            loader.ensure_extension()

    def test_custom_s3_base_url(self):
        loader = AnofoxLoader(
            s3_base_url="https://custom.example.com/",
            duckdb_version="1.0.0",
        )
        loader._arch = "linux_amd64"
        assert loader._candidate_urls()[1] == (
            "https://custom.example.com/v1.0.0/linux_amd64/anofox_tabular.duckdb_extension.gz"
        )

    def test_ensure_extension_requires_a_checksum_sidecar_by_default(self, tmp_path):
        """Without a sidecar the candidate is skipped and the error explains the opt-out."""
        with patch("pathlib.Path.home", return_value=tmp_path):
            loader = AnofoxLoader(duckdb_version="9.9.9", allow_insecure_download=False)
            loader._arch = "linux_amd64"
            with patch("anofox._loader._fetch_sha256", side_effect=OSError("HTTP Error 404")), \
                 patch("anofox._loader._download") as download:
                with pytest.raises(RuntimeError) as excinfo:
                    loader.ensure_extension()
        download.assert_not_called()
        assert "no checksum sidecar" in str(excinfo.value)
        assert "ANOFOX_ALLOW_INSECURE_DOWNLOAD=1" in str(excinfo.value)

    def test_ensure_extension_caches_digest_of_decompressed_payload(self, tmp_path):
        """A verified download leaves a checksum file that matches the cached binary."""
        payload = b"extension bytes"
        archive = gzip.compress(payload)
        with patch("pathlib.Path.home", return_value=tmp_path):
            loader = AnofoxLoader(duckdb_version="9.9.9", allow_insecure_download=False)
            loader._arch = "linux_amd64"
            with patch("anofox._loader._fetch_sha256", return_value=hashlib.sha256(archive).hexdigest()), \
                 patch("urllib.request.urlopen", return_value=_FakeResponse(archive)):
                result = loader.ensure_extension()
            cached = loader._cached_path()
            assert result == str(cached)
            assert cached.read_bytes() == payload
            assert loader._cached_checksum_path().read_text() == hashlib.sha256(payload).hexdigest()
            # Second call is served from the cache and still verifies.
            with patch("urllib.request.urlopen", side_effect=AssertionError("must not download")):
                assert loader.ensure_extension() == str(cached)

    def test_forecast_extension_urls(self):
        """Demonstrate reusability for sibling extensions."""
        loader = AnofoxLoader(
            extension_name="anofox_forecast",
            s3_base_url="https://get.erpl.io",
            duckdb_version="1.1.3",
        )
        loader._arch = "linux_amd64"
        assert loader._candidate_urls() == [
            "https://community-extensions.duckdb.org/v1.1.3/linux_amd64/anofox_forecast.duckdb_extension.gz",
            "https://get.erpl.io/v1.1.3/linux_amd64/anofox_forecast.duckdb_extension.gz",
        ]


class _FakeResponse:
    """Minimal stand-in for the context manager returned by urlopen."""

    def __init__(self, body: bytes) -> None:
        self._body = body

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self) -> bytes:
        return self._body


class TestDownload:
    def test_download_verifies_the_compressed_archive_and_writes_the_payload(self, tmp_path):
        payload = b"payload"
        archive = gzip.compress(payload)
        dest = tmp_path / "ext.duckdb_extension"
        with patch("urllib.request.urlopen", return_value=_FakeResponse(archive)):
            digest = _download(
                "https://example.com/ext.duckdb_extension.gz",
                dest,
                expected_sha256=hashlib.sha256(archive).hexdigest(),
            )
        assert dest.read_bytes() == payload
        assert digest == hashlib.sha256(payload).hexdigest()

    def test_download_rejects_a_tampered_archive(self, tmp_path):
        archive = gzip.compress(b"payload")
        dest = tmp_path / "ext.duckdb_extension"
        with patch("urllib.request.urlopen", return_value=_FakeResponse(archive)):
            with pytest.raises(RuntimeError, match="SHA256 mismatch"):
                _download("https://example.com/ext.duckdb_extension.gz", dest, expected_sha256="0" * 64)
        assert not dest.exists()


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


# ---------------------------------------------------------------------------
# load_into: DuckDB's own community install first, then the S3 download
# ---------------------------------------------------------------------------

class _FakeResult:
    def __init__(self, rows):
        self._rows = rows

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)


class _FakeConnection:
    """Records the SQL a loader runs; statements matching a key in *fail* raise."""

    def __init__(self, unsigned=False, fail=(), install_path="/duckdb/ext/anofox_tabular.duckdb_extension"):
        self.statements: list[str] = []
        self.closed = False
        self._unsigned = unsigned
        self._fail = fail
        self._install_path = install_path

    def execute(self, query, parameters=None):
        self.statements.append(query)
        for fragment in self._fail:
            if fragment in query:
                raise RuntimeError(f"fake failure: {query}")
        if "current_setting('allow_unsigned_extensions')" in query:
            return _FakeResult([(self._unsigned,)])
        if "install_path" in query:
            return _FakeResult([(self._install_path,)])
        return _FakeResult([])

    def close(self):
        self.closed = True


def _loader(tmp_path, **kwargs):
    kwargs.setdefault("duckdb_version", "2.0.0")
    loader = AnofoxLoader(**kwargs)
    loader._arch = "linux_amd64"
    return loader


class TestLoadInto:
    def test_local_path_wins_and_skips_every_repository(self, tmp_path):
        ext_file = tmp_path / "it's.duckdb_extension"
        ext_file.write_bytes(b"fake")
        conn = _FakeConnection()
        loader = _loader(tmp_path)
        with patch.object(AnofoxLoader, "_download_extension", side_effect=AssertionError("no download")):
            source = loader.load_into(conn, local_path=str(ext_file))
        assert source == "local"
        assert conn.statements == [f"LOAD '{str(ext_file.resolve()).replace(chr(39), chr(39) * 2)}'"]

    def test_community_install_is_tried_first(self, tmp_path):
        conn = _FakeConnection(unsigned=False)
        loader = _loader(tmp_path)
        with patch.object(AnofoxLoader, "_download_extension", side_effect=AssertionError("no download")):
            source = loader.load_into(conn)
        assert source == "community"
        assert "INSTALL anofox_tabular FROM community" in conn.statements
        assert conn.statements[-1] == "LOAD anofox_tabular"

    def test_unsigned_connection_verifies_the_signature_on_a_separate_database(self, tmp_path):
        """allow_unsigned_extensions would skip DuckDB's signature check, so a default-config
        database installs and loads it first; the user connection then loads that file."""
        conn = _FakeConnection(unsigned=True)
        verifier = _FakeConnection(install_path="/home/u/.duckdb/extensions/v2.0.0/linux_amd64/anofox_tabular.duckdb_extension")
        loader = _loader(tmp_path)
        with patch("anofox._loader._open_verifier_connection", return_value=verifier), \
             patch.object(AnofoxLoader, "_download_extension", side_effect=AssertionError("no download")):
            source = loader.load_into(conn)
        assert source == "community"
        assert "INSTALL anofox_tabular FROM community" in verifier.statements
        assert "LOAD anofox_tabular" in verifier.statements
        assert verifier.closed
        assert not any("INSTALL" in s for s in conn.statements)
        assert conn.statements[-1] == (
            "LOAD '/home/u/.duckdb/extensions/v2.0.0/linux_amd64/anofox_tabular.duckdb_extension'"
        )

    def test_failed_signature_check_falls_back_to_the_download(self, tmp_path):
        conn = _FakeConnection(unsigned=True)
        verifier = _FakeConnection(fail=("LOAD anofox_tabular",))
        downloaded = tmp_path / "dl.duckdb_extension"
        loader = _loader(tmp_path)
        with patch("anofox._loader._open_verifier_connection", return_value=verifier), \
             patch.object(AnofoxLoader, "_download_extension", return_value=(str(downloaded), [])):
            source = loader.load_into(conn)
        assert source == "download"
        assert verifier.closed
        assert conn.statements[-1] == f"LOAD '{downloaded}'"

    @pytest.mark.parametrize("failing", ["INSTALL anofox_tabular", "LOAD anofox_tabular"])
    def test_community_failure_falls_back_to_the_verified_download(self, tmp_path, failing):
        """Offline, no build for this platform/version, or a load error: use get.erpl.io."""
        conn = _FakeConnection(fail=(failing,))
        downloaded = tmp_path / "dl.duckdb_extension"
        loader = _loader(tmp_path)
        with patch.object(AnofoxLoader, "_download_extension", return_value=(str(downloaded), [])) as dl:
            source = loader.load_into(conn)
        dl.assert_called_once()
        assert source == "download"
        assert conn.statements[-1] == f"LOAD '{downloaded}'"

    def test_all_sources_failing_raises_one_clear_error(self, tmp_path):
        conn = _FakeConnection(fail=("INSTALL anofox_tabular",))
        loader = _loader(tmp_path)
        with patch.object(
            AnofoxLoader,
            "_download_extension",
            return_value=(None, ["https://get.erpl.io/v2.0.0/x.gz: HTTP Error 404"]),
        ):
            with pytest.raises(RuntimeError) as excinfo:
                loader.load_into(conn)
        message = str(excinfo.value)
        assert "Could not find or download extension 'anofox_tabular'" in message
        assert "ANOFOX_EXT_PATH" in message
        assert "INSTALL anofox_tabular FROM community" in message
        assert "HTTP Error 404" in message

    def test_explicit_s3_base_url_is_tried_before_the_community_repository(self, tmp_path):
        conn = _FakeConnection()
        downloaded = tmp_path / "dl.duckdb_extension"
        loader = _loader(tmp_path, s3_base_url="https://mirror.example.com")
        with patch.object(AnofoxLoader, "_download_extension", return_value=(str(downloaded), [])):
            source = loader.load_into(conn)
        assert source == "download"
        assert not any("INSTALL" in s for s in conn.statements)

    def test_explicit_s3_base_url_still_falls_back_to_the_community_repository(self, tmp_path):
        conn = _FakeConnection()
        loader = _loader(tmp_path, s3_base_url="https://mirror.example.com")
        with patch.object(AnofoxLoader, "_download_extension", return_value=(None, ["mirror: 404"])):
            source = loader.load_into(conn)
        assert source == "community"

    @pytest.mark.parametrize("version", ["2.0.0rc1", "1.5.7.dev42"])
    def test_prerelease_raises_before_any_install(self, tmp_path, version):
        conn = _FakeConnection()
        loader = _loader(tmp_path, duckdb_version=version)
        with pytest.raises(UnsupportedDuckDBVersionError):
            loader.load_into(conn)
        assert conn.statements == []


@pytest.mark.skipif(
    os.environ.get("ANOFOX_TEST_NETWORK") != "1",
    reason="network test; set ANOFOX_TEST_NETWORK=1 to run",
)
@pytest.mark.parametrize("allow_unsigned", [True, False])
def test_connect_without_a_local_build_loads_from_the_network(tmp_path, monkeypatch, allow_unsigned):
    """A fresh machine (empty HOME, no ANOFOX_EXT_PATH) gets a working extension."""
    import anofox

    monkeypatch.delenv("ANOFOX_EXT_PATH", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    with patch("pathlib.Path.home", return_value=tmp_path):
        conn = anofox.connect(allow_unsigned=allow_unsigned)
    try:
        assert conn._extension_source in {"community", "download"}
        assert conn.execute("SELECT anofox_tab_vat_is_valid('DE111111125')").fetchall() == [(True,)]
    finally:
        conn.close()
