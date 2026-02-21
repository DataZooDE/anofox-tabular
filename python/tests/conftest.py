"""
Pytest fixtures for anofox Python package tests.

The ``conn`` fixture requires a locally-built extension binary.
Set ANOFOX_EXT_PATH to point at the binary, or build first:

    make release  # from repo root
    export ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension

Tests that depend on ``conn`` are automatically skipped when the binary is absent.
"""

import os
from pathlib import Path

import pytest


# ---------------------------------------------------------------------------
# Extension path fixture (session-scoped so the binary is resolved once)
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def ext_path() -> str | None:
    """
    Return the path to the extension binary, or None if not available.

    Resolution order:
    1. ANOFOX_EXT_PATH environment variable
    2. Default build output path relative to the repo root
    """
    env_path = os.environ.get("ANOFOX_EXT_PATH")
    if env_path and Path(env_path).is_file():
        return env_path

    # Try the default build path (relative to repo root, two levels up from python/tests/)
    repo_root = Path(__file__).parent.parent.parent
    default = repo_root / "build" / "release" / "extension" / "anofox_tabular" / "anofox_tabular.duckdb_extension"
    if default.is_file():
        return str(default)

    return None


# ---------------------------------------------------------------------------
# Connection fixture
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def conn(ext_path):
    """
    Return a live AnofoxConnection using the locally-built extension.

    Skips the test if no extension binary is available.
    """
    if ext_path is None:
        pytest.skip(
            "Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'."
        )

    import sys
    sys.path.insert(0, str(Path(__file__).parent.parent / "src"))
    import anofox

    connection = anofox.connect(extension_path=ext_path, allow_unsigned=True)
    yield connection
    connection.close()
