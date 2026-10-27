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


def quote_table_name(name: str) -> str:
    """
    Quote a table name that may be qualified: ``"dq.checks"`` becomes ``"dq"."checks"``.

    *name* is split at each dot into one to three parts (``table``,
    ``schema.table``, ``catalog.schema.table``), each quoted with
    :func:`quote_ident`, so the table is created in or read from that schema
    instead of a table literally named ``dq.checks`` in the default schema. A
    name with an empty part or more than three parts is quoted as one
    identifier.
    """
    parts = quote_ident(name)[1:-1].split(".")
    if not 1 <= len(parts) <= 3 or not all(parts):
        return quote_ident(name)
    return ".".join(f'"{part}"' for part in parts)


def finish(conn: Any) -> None:
    """
    Run the pending result of ``execute()`` on *conn* to its end.

    The DuckDB 2.0 Python client keeps a query result open until it is read to
    the end; while one is open, ``register()``, ``unregister()`` and ``sql()``
    on the same connection raise ``ResourceInUseException``. (``fetchone()``
    on a one-row result does not end it: only a fetch that returns no row
    does.) DuckDB 1.x materialises every result and has no ``complete()``, so
    this is a no-op there. *conn* is a DuckDB connection or an
    :class:`~anofox.AnofoxConnection`.
    """
    complete = getattr(getattr(conn, "native", conn), "complete", None)
    if complete is not None:
        complete()


def run(conn: Any, query: str, params: Sequence[Any] = ()) -> None:
    """Run *query* with bound *params* for its effect, leaving no pending result."""
    conn.execute(query, list(params))
    finish(conn)


def fetch_row(conn: Any, query: str, params: Sequence[Any] = ()) -> Optional[tuple]:
    """Run *query* with bound *params* and return its first row (``None`` if empty)."""
    row = conn.execute(query, list(params)).fetchone()
    finish(conn)
    return row


def fetch_one(conn: Any, query: str, params: Sequence[Any] = ()) -> Any:
    """Run *query* with bound *params* and return the first column of the first row."""
    row = fetch_row(conn, query, params)
    return None if row is None else row[0]


def fetch_frame(conn: Any, query: str, params: Sequence[Any] = (), df_type: str = "pandas") -> Any:
    """Run *query* with bound *params* and return the result as a pandas or polars DataFrame."""
    # A relation from sql() offers .df() and .pl() on DuckDB 1.x and 2.0; the
    # result of execute() has no .pl() on 2.0.
    native = getattr(conn, "native", conn)
    finish(native)
    relation = native.sql(query, params=list(params))
    return relation.pl() if df_type == "polars" else relation.df()


def id_column_arg(id_column: Optional[str], params: list) -> str:
    """Return the ``, id_column := ?`` named argument (appending its value to *params*)."""
    if id_column is None:
        return ""
    params.append(id_column)
    return ", id_column := ?"
