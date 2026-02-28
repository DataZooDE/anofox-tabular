"""
anofox — Python wrapper for the anofox-tabular DuckDB extension.

Quick start::

    import anofox

    with anofox.connect() as conn:
        print(conn.execute("SELECT anofox_tab_email_is_valid('hi@example.com')").fetchone())

Or with a local dev build::

    conn = anofox.connect(
        extension_path="../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension"
    )
"""

from __future__ import annotations

import os
from typing import Optional

from ._connection import AnofoxConnection
from ._loader import _DEFAULT_S3_BASE

__all__ = ["connect", "AnofoxConnection", "profile"]


def connect(
    database: str = ":memory:",
    *,
    duckdb_version: Optional[str] = None,
    extension_path: Optional[str] = None,
    allow_unsigned: bool = True,
    s3_base_url: str = _DEFAULT_S3_BASE,
) -> AnofoxConnection:
    """
    Open a DuckDB connection with the anofox_tabular extension loaded.

    Parameters
    ----------
    database:
        Path to a DuckDB database file, or ``":memory:"`` (default).
    duckdb_version:
        Override the DuckDB version used for extension lookup.
        Defaults to the version of the installed ``duckdb`` package.
    extension_path:
        Absolute path to a locally-built ``.duckdb_extension`` binary.
        When provided, skips all download logic.  The environment variable
        ``ANOFOX_EXT_PATH`` is also checked as a fallback.
    allow_unsigned:
        Pass ``allow_unsigned_extensions = true`` to DuckDB (required for
        locally-built or community extensions).  Default: ``True``.
    s3_base_url:
        Override the S3 mirror base URL used for downloading.

    Returns
    -------
    AnofoxConnection
    """
    # Environment variable takes precedence over the auto-download logic
    # but is overridden by an explicit argument.
    if extension_path is None:
        extension_path = os.environ.get("ANOFOX_EXT_PATH")

    return AnofoxConnection(
        database=database,
        extension_path=extension_path,
        allow_unsigned=allow_unsigned,
        duckdb_version=duckdb_version,
        s3_base_url=s3_base_url,
    )
