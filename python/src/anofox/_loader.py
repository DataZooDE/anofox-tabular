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

        # Try to download
        cached.parent.mkdir(parents=True, exist_ok=True)
        errors: list[str] = []
        for url in self._candidate_urls():
            try:
                expected_sha = None
                if not self._allow_insecure_download:
                    expected_sha = _fetch_sha256(f"{url}.sha256")
                _download(url, cached, expected_sha256=expected_sha)
                if expected_sha:
                    self._cached_checksum_path().write_text(expected_sha)
                return str(cached)
            except Exception as exc:
                errors.append(f"{url}: {exc}")
                continue

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
        """Return download URLs to try, in priority order."""
        gz_name = f"{self._extension_name}.duckdb_extension.gz"
        return [
            # Community registry (official DuckDB community extensions)
            f"{_COMMUNITY_REGISTRY_BASE}/v1/{self._duckdb_version}/{self._arch}/{self._extension_name}/{gz_name}",
            # Direct S3 mirror (latest)
            f"{self._s3_base_url}/{self._duckdb_version}/{self._arch}/{gz_name}",
        ]


# ---------------------------------------------------------------------------
# Module-level helpers
# ---------------------------------------------------------------------------

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


def _download(url: str, destination: Path, expected_sha256: Optional[str] = None) -> None:
    """
    Download *url* to *destination*, decompressing gzip on the fly.

    Raises :class:`urllib.error.URLError` or :class:`OSError` on failure.
    """
    tmp = destination.with_suffix(".tmp")
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()

        # Decompress if the URL ends with .gz
        if url.endswith(".gz"):
            data = gzip.decompress(data)

        if expected_sha256:
            _verify_sha256(data, expected_sha256)

        tmp.write_bytes(data)
        shutil.move(str(tmp), str(destination))
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
