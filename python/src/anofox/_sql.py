"""
SQL building helpers.

Values always travel as bound query parameters (``?``); only identifiers
(column and table names inside a statement) are spliced into the SQL text,
and only through :func:`quote_ident`.
"""

from __future__ import annotations

from typing import Any, Optional, Sequence


def quote_ident(name: str) -> str:
    """Quote *name* as a SQL identifier (double quotes, embedded quotes doubled)."""
    if not isinstance(name, str):
        raise TypeError(f"identifier must be a string, got {type(name).__qualname__!r}")
    return '"' + name.replace('"', '""') + '"'


def fetch_one(conn: Any, query: str, params: Sequence[Any] = ()) -> Any:
    """Run *query* with bound *params* and return the first column of the first row."""
    row = conn.execute(query, list(params)).fetchone()
    return None if row is None else row[0]


def fetch_frame(conn: Any, query: str, params: Sequence[Any] = (), df_type: str = "pandas") -> Any:
    """Run *query* with bound *params* and return the result as a pandas or polars DataFrame."""
    result = conn.native.execute(query, list(params))
    return result.pl() if df_type == "polars" else result.df()


def id_column_arg(id_column: Optional[str], params: list) -> str:
    """Return the ``, id_column := ?`` named argument (appending its value to *params*)."""
    if id_column is None:
        return ""
    params.append(id_column)
    return ", id_column := ?"
