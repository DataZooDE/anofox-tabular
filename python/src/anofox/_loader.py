"""
Extension loader with platform detection, download, and caching.

Designed to be reusable across the DataZooDE extension ecosystem
(anofox-tabular, anofox-forecast, anofox-statistics).
"""

from __future__ import annotations

import gzip
import hashlib
import importlib.metadata
import os
import platform
import re
import shutil
import urllib.request
from pathlib import Path
from typing import Optional


_COMMUNITY_REGISTRY_BASE = "https://community-extensions.duckdb.org"
_DEFAULT_S3_BASE = "https://get.erpl.io"
_SHA256_PATTERN = re.compile(r"\b([0-9a-fA-F]{64})\b")
# Extension builds are published per DuckDB release only, under the release
# tag: "1.5.6" -> v1.5.6, "2.0.0" -> v2.0.0. Pre-, dev- and post-releases
# (2.0.0rc1, 1.5.7.dev42, 1.5.6.post1, local "+..." builds) have no build.
_RELEASE_VERSION_PATTERN = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")


class UnsupportedDuckDBVersionError(RuntimeError):
    """The installed DuckDB version has no published extension build."""


class AnofoxLoader:
    """
    Handles locating, downloading, and caching a DuckDB extension binary.

    Parameters
    ----------
    extension_name:
        Name of the extension, e.g. ``"anofox_tabular"``.
    s3_base_url:
        Public S3 mirror base URL, e.g. ``"https://get.erpl.io"``.
    duckdb_version:
        Override the DuckDB version string.  When *None* the version is
        detected from the installed ``duckdb`` package.
    """

    def __init__(
        self,
        extension_name: str = "anofox_tabular",
        s3_base_url: str = _DEFAULT_S3_BASE,
        duckdb_version: Optional[str] = None,
        allow_insecure_download: Optional[bool] = None,
    ) -> None:
        self._extension_name = extension_name
        self._s3_base_url = s3_base_url.rstrip("/")
        self._duckdb_version = duckdb_version or _detect_duckdb_version()
        self._arch = _detect_platform()
        if allow_insecure_download is None:
            self._allow_insecure_download = _is_truthy_env(
                os.environ.get("ANOFOX_ALLOW_INSECURE_DOWNLOAD")
            )
        else:
            self._allow_insecure_download = allow_insecure_download

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def ensure_extension(self, local_path: Optional[str] = None) -> str:
        """
        Return an absolute path to the extension binary, downloading if needed.

        Downloads exist only for DuckDB releases (``1.5.6``, ``2.0.0``); a
        pre-, dev- or post-release ``duckdb`` raises
        :class:`UnsupportedDuckDBVersionError` before any network request.
        *local_path* (``ANOFOX_EXT_PATH``) works with every version.

        Resolution order:
        1. *local_path* (if provided and the file exists)
        2. Cached path under ``~/.anofox/``
        3. Community registry download
        4. S3 direct download (``self._s3_base_url``)

        Returns the resolved path as a string.
        Raises :class:`RuntimeError` if the extension cannot be found.
        """
        if local_path and Path(local_path).is_file():
            return str(Path(local_path).resolve())

        cached = self._cached_path()
        if cached.is_file():
            if self._allow_insecure_download:
                return str(cached)
            checksum_path = self._cached_checksum_path()
            if checksum_path.is_file():
                _verify_sha256(cached.read_bytes(), checksum_path.read_text().strip())
                return str(cached)
            # Existing cache without a checksum is treated as untrusted.
            cached.unlink(missing_ok=True)

        # Resolve the URLs first: a DuckDB version without published builds
        # fails here with an actionable error, before any network request.
        urls = self._candidate_urls()

        # Try to download
        cached.parent.mkdir(parents=True, exist_ok=True)
        errors: list[str] = []
        for url in urls:
            expected_sha = None
            if not self._allow_insecure_download:
                # The sidecar holds the digest of the compressed archive as
                # published (``sha256sum <name>.duckdb_extension.gz``).
                try:
                    expected_sha = _fetch_sha256(f"{url}.sha256")
                except Exception as exc:
                    errors.append(
                        f"{url}: no checksum sidecar available ({exc}); set "
                        "ANOFOX_ALLOW_INSECURE_DOWNLOAD=1 to download without verification"
                    )
                    continue
            try:
                payload_sha = _download(url, cached, expected_sha256=expected_sha)
            except Exception as exc:
                errors.append(f"{url}: {exc}")
                continue
            if expected_sha:
                # Cache hits are verified against the *decompressed* binary.
                self._cached_checksum_path().write_text(payload_sha)
            return str(cached)

        raise RuntimeError(
            f"Could not find or download extension '{self._extension_name}'. "
            "Set ANOFOX_EXT_PATH to a local build path, or ensure network access. "
            "To bypass checksum enforcement, set ANOFOX_ALLOW_INSECURE_DOWNLOAD=1. "
            f"Download errors: {'; '.join(errors)}"
        )

    @property
    def extension_name(self) -> str:
        return self._extension_name

    @property
    def duckdb_version(self) -> str:
        return self._duckdb_version

    @property
    def arch(self) -> str:
        return self._arch

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _cached_path(self) -> Path:
        base = Path.home() / ".anofox" / "extensions"
        return (
            base
            / self._extension_name
            / self._duckdb_version
            / self._arch
            / f"{self._extension_name}.duckdb_extension"
        )

    def _cached_checksum_path(self) -> Path:
        return self._cached_path().with_suffix(".duckdb_extension.sha256")

    def _candidate_urls(self) -> list[str]:
        """
        Return download URLs to try, in priority order.

        Both sources use DuckDB's extension repository layout,
        ``<base>/<duckdb tag>/<platform>/<name>.duckdb_extension.gz``, where
        the DuckDB tag carries a ``v`` prefix (``v1.5.6``) while the installed
        ``duckdb`` package reports ``1.5.6``.
        """
        gz_name = f"{self._extension_name}.duckdb_extension.gz"
        tag = _duckdb_tag(self._duckdb_version)
        return [
            # Community registry (official DuckDB community extensions)
            f"{_COMMUNITY_REGISTRY_BASE}/{tag}/{self._arch}/{gz_name}",
            # Direct S3 mirror (latest build from main)
            f"{self._s3_base_url}/{tag}/{self._arch}/{gz_name}",
        ]


# ---------------------------------------------------------------------------
# Module-level helpers
# ---------------------------------------------------------------------------

def _duckdb_tag(version: str) -> str:
    """
    Map a DuckDB release version (``"1.5.6"``, ``"2.0.0"``) to its repository
    tag (``"v1.5.6"``, ``"v2.0.0"``).

    Raises :class:`UnsupportedDuckDBVersionError` for any other version
    string, e.g. ``"2.0.0rc1"``, ``"1.5.7.dev42"`` or ``"1.5.6.post1"``:
    extensions are only published for DuckDB releases.
    """
    match = _RELEASE_VERSION_PATTERN.match(version.strip())
    if not match:
        raise UnsupportedDuckDBVersionError(
            f"duckdb {version!r} is not a DuckDB release, and extension binaries are "
            "only published for releases (e.g. 1.5.6 or 2.0.0). Install a released "
            "duckdb package, or point ANOFOX_EXT_PATH (or connect(extension_path=...)) "
            "at an extension built for this DuckDB version."
        )
    major, minor, patch = match.groups()
    return f"v{major}.{minor}.{patch}"


def _detect_duckdb_version() -> str:
    """Return the installed DuckDB version string, e.g. ``"1.1.3"``."""
    try:
        return importlib.metadata.version("duckdb")
    except importlib.metadata.PackageNotFoundError:
        raise RuntimeError("duckdb is not installed; cannot detect version.")


def _detect_platform() -> str:
    """
    Map the current OS + CPU to a DuckDB platform string.

    Examples: ``"linux_amd64"``, ``"osx_arm64"``, ``"windows_amd64"``.
    """
    system = platform.system().lower()
    machine = platform.machine().lower()

    _machine_map: dict[str, str] = {
        "x86_64": "amd64",
        "amd64": "amd64",
        "aarch64": "arm64",
        "arm64": "arm64",
    }
    arch = _machine_map.get(machine, machine)

    _system_map: dict[str, str] = {
        "linux": "linux",
        "darwin": "osx",
        "windows": "windows",
    }
    os_name = _system_map.get(system, system)

    return f"{os_name}_{arch}"


def _download(url: str, destination: Path, expected_sha256: Optional[str] = None) -> str:
    """
    Download *url* to *destination*, decompressing gzip on the fly.

    *expected_sha256* is checked against the bytes as downloaded (the
    compressed archive), matching a ``sha256sum`` sidecar published next to
    it.  Returns the SHA256 of the decompressed payload that was written.

    Raises :class:`urllib.error.URLError` or :class:`OSError` on failure.
    """
    tmp = destination.with_suffix(".tmp")
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()

        if expected_sha256:
            _verify_sha256(data, expected_sha256)

        # Decompress if the URL ends with .gz
        if url.endswith(".gz"):
            data = gzip.decompress(data)

        tmp.write_bytes(data)
        shutil.move(str(tmp), str(destination))
        return hashlib.sha256(data).hexdigest()
    finally:
        if tmp.exists():
            tmp.unlink(missing_ok=True)


def _fetch_sha256(url: str) -> str:
    """Fetch a SHA256 sidecar file and return the parsed digest."""
    with urllib.request.urlopen(url, timeout=30) as response:
        text = response.read().decode("utf-8", errors="replace")

    match = _SHA256_PATTERN.search(text)
    if not match:
        raise RuntimeError(f"Could not parse SHA256 from {url!r}")
    return match.group(1).lower()


def _verify_sha256(data: bytes, expected_sha256: str) -> None:
    actual = hashlib.sha256(data).hexdigest().lower()
    if actual != expected_sha256.lower():
        raise RuntimeError(
            f"SHA256 mismatch: expected {expected_sha256.lower()}, got {actual}"
        )


def _is_truthy_env(value: Optional[str]) -> bool:
    if value is None:
        return False
    return value.strip().lower() in {"1", "true", "yes", "on"}
