"""
AnofoxConnection — thin wrapper around a DuckDB connection with the
anofox_tabular extension pre-loaded.
"""

from __future__ import annotations

from typing import Any, Optional

import duckdb

from ._loader import AnofoxLoader, _DEFAULT_S3_BASE
from ._sql import fetch_one, finish, quote_ident


class AnofoxConnection:
    """
    A DuckDB connection with the anofox_tabular extension loaded.

    Use :func:`anofox.connect` to create instances rather than calling
    this constructor directly.

    Supports the context-manager protocol::

        with anofox.connect() as conn:
            result = conn.execute("SELECT anofox_tab_email_is_valid('hi@example.com')")
    """

    def __init__(
        self,
        database: str = ":memory:",
        extension_path: Optional[str] = None,
        allow_unsigned: bool = True,
        duckdb_version: Optional[str] = None,
        s3_base_url: str = _DEFAULT_S3_BASE,
    ) -> None:
        config: dict[str, Any] = {}
        if allow_unsigned:
            config["allow_unsigned_extensions"] = "true"

        self._conn = duckdb.connect(database, config=config)

        loader = AnofoxLoader(
            extension_name="anofox_tabular",
            s3_base_url=s3_base_url,
            duckdb_version=duckdb_version,
        )
        resolved_path = loader.ensure_extension(local_path=extension_path)
        escaped_path = str(resolved_path).replace("'", "''")
        self._conn.execute(f"LOAD '{escaped_path}'")

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self) -> "AnofoxConnection":
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Delegation to underlying duckdb connection
    # ------------------------------------------------------------------

    def execute(self, query: str, parameters: Any = None) -> Any:
        """
        Execute a SQL statement and return the DuckDB result object.

        On DuckDB 2.0 the result stays open until a fetch returns no more
        rows (``fetchone()`` on a one-row result does not end it). The other
        methods of this class, and the anofox functions taking this
        connection, first run such a pending result to its end, so a partly
        read result never blocks them.
        """
        if parameters is not None:
            return self._conn.execute(query, parameters)
        return self._conn.execute(query)

    def complete(self) -> None:
        """Run the pending result of :meth:`execute` to its end (no-op on DuckDB 1.x)."""
        finish(self._conn)

    def sql(self, query: str) -> Any:
        """Execute a SQL statement and return a DuckDB relation."""
        finish(self._conn)
        return self._conn.sql(query)

    def register(self, name: str, df: Any) -> None:
        """Register a DataFrame as a named view."""
        finish(self._conn)
        self._conn.register(name, df)

    def unregister(self, name: str) -> None:
        """Remove a previously registered view."""
        finish(self._conn)
        self._conn.unregister(name)

    def close(self) -> None:
        """Close the underlying DuckDB connection."""
        self._conn.close()

    @property
    def native(self) -> duckdb.DuckDBPyConnection:
        """Access the underlying DuckDB connection directly."""
        return self._conn

    # ------------------------------------------------------------------
    # High-level convenience methods
    # ------------------------------------------------------------------

    def is_extension_loaded(self) -> bool:
        """Return True if the anofox_tabular extension is loaded."""
        count = fetch_one(
            self._conn,
            "SELECT count(*) FROM duckdb_functions() WHERE function_name LIKE 'anofox_%'",
        )
        return count is not None and count > 0

    def profile(self, df_or_table: Any) -> Any:
        """
        Profile a DataFrame or named table.

        Returns a DataFrame with per-column quality metrics: null_rate,
        distinct_count, and row volume. The DataFrame is a polars DataFrame
        when *df_or_table* is one, else a pandas DataFrame.

        Parameters
        ----------
        df_or_table:
            Either a pandas/polars DataFrame or a string table name.
        """
        from ._utils import detect_df_type, drop_df_table, register_df_as_table

        if isinstance(df_or_table, str):
            table_name = df_or_table
            registered = False
            df_type = "pandas"
        else:
            df_type = detect_df_type(df_or_table)
            table_name = register_df_as_table(self._conn, df_or_table)
            registered = True

        try:
            # Get column names and types
            cols_result = self._conn.execute(
                "SELECT column_name, data_type FROM information_schema.columns "
                "WHERE table_name = ? ORDER BY ordinal_position",
                [table_name],
            ).fetchall()

            # A plain name refers to a table in the search path; anything else
            # (e.g. "schema.table") is used as written.
            table_sql = quote_ident(table_name) if cols_result else table_name
            if not cols_result:
                # Fallback: describe the table
                desc = self._conn.execute(f"DESCRIBE {table_sql}").fetchall()
                cols_result = [(row[0], row[1]) for row in desc]

            # Volume (total rows)
            total = fetch_one(self._conn, f"SELECT count(*) FROM {table_sql}")

            rows = []
            for col_name, col_type in cols_result:
                col_sql = quote_ident(col_name)
                null_count = fetch_one(
                    self._conn, f"SELECT count(*) FROM {table_sql} WHERE {col_sql} IS NULL"
                )
                distinct_count = fetch_one(
                    self._conn, f"SELECT count(DISTINCT {col_sql}) FROM {table_sql}"
                )

                null_rate = null_count / total if total > 0 else 0.0
                rows.append({
                    "column": col_name,
                    "type": col_type,
                    "row_count": total,
                    "null_count": null_count,
                    "null_rate": round(null_rate, 4),
                    "distinct_count": distinct_count,
                    "distinct_rate": round(distinct_count / total if total > 0 else 0.0, 4),
                })

            return _rows_to_frame(
                rows,
                ["column", "type", "row_count", "null_count", "null_rate",
                 "distinct_count", "distinct_rate"],
                df_type,
            )
        finally:
            if registered:
                drop_df_table(self._conn, table_name)

    def validate(self, df: Any, schema: dict) -> "ValidationResult":
        """
        Validate a DataFrame against a schema of rules.

        Parameters
        ----------
        df:
            pandas or polars DataFrame.
        schema:
            Mapping of ``{column_name: rule}`` where each rule is an
            :class:`EmailRule` or :class:`PhoneRule`.

        Returns
        -------
        ValidationResult
            Contains ``passed`` (bool) and ``failures``, a DataFrame (polars
            for a polars *df*, else pandas) with the
            columns ``column``, ``row_id``, ``value`` and ``rule``. ``row_id``
            identifies the failing row of *df*: its index label for a pandas
            DataFrame, its 0-based row position for a polars DataFrame.
        """
        from .validate import EmailRule, PhoneRule
        from ._utils import POSITION_COLUMN, detect_df_type, registered_view

        df_type = detect_df_type(df)
        labels = list(df.index) if df_type == "pandas" else None

        failure_rows = []
        with registered_view(self._conn, df) as view:
            for col, rule in schema.items():
                if isinstance(rule, EmailRule):
                    mode = getattr(rule, "mode", "regex")
                    check_sql, param, rule_name = "anofox_tab_email_is_valid", mode, f"email({mode})"
                elif isinstance(rule, PhoneRule):
                    region = getattr(rule, "region", "US")
                    check_sql, param, rule_name = "anofox_tab_phonenumber_is_valid", region, f"phone({region})"
                else:
                    continue
                results = self._conn.execute(
                    f"SELECT {quote_ident(POSITION_COLUMN)}, {quote_ident(col)}, "
                    f"{check_sql}({quote_ident(col)}, ?) AS valid "
                    f"FROM {quote_ident(view)} ORDER BY 1",
                    [param],
                ).fetchall()
                for position, value, valid in results:
                    if not valid:
                        failure_rows.append({
                            "column": col,
                            "row_id": labels[position] if labels is not None else position,
                            "value": value,
                            "rule": rule_name,
                        })

        failures_df = _rows_to_frame(failure_rows, ["column", "row_id", "value", "rule"], df_type)
        return ValidationResult(passed=len(failure_rows) == 0, failures=failures_df)

    def quality_check(self, df: Any, rules: dict) -> "QualityResult":
        """
        Run quality checks on a DataFrame.

        Parameters
        ----------
        df:
            pandas or polars DataFrame.
        rules:
            Mapping of ``{check_name: params_dict}``.  Supported checks:
            ``"volume"``, ``"null_rate"``, ``"distinct_count"``; any other
            name raises :class:`ValueError`.

        Returns
        -------
        dict
            ``{"status": "pass"|"fail", "checks": {check_name: result_dict}}``.
        """
        from ._utils import detect_df_type, drop_df_table, register_df_as_table
        from . import quality as q

        detect_df_type(df)  # validate type
        unknown = [name for name in rules if name not in _QUALITY_CHECK_RULES]
        if unknown:
            raise ValueError(
                f"unknown quality_check rule(s): {', '.join(map(repr, unknown))}; "
                f"supported: {', '.join(map(repr, _QUALITY_CHECK_RULES))}. "
                "Use anofox.quality for the other checks."
            )
        table_name = register_df_as_table(self._conn, df)

        results = {}
        try:
            for check_name, params in rules.items():
                if check_name == "volume":
                    results[check_name] = q.volume(
                        self, table_name,
                        min_rows=params.get("min_rows", 1),
                        max_rows=params.get("max_rows"),
                    )
                elif check_name == "null_rate":
                    results[check_name] = q.null_rate(
                        self, table_name,
                        column_name=params["column"],
                        max_null_rate=params.get("max_null_rate", 0.0),
                    )
                elif check_name == "distinct_count":
                    results[check_name] = q.distinct_count(
                        self, table_name,
                        column_name=params["column"],
                        min_distinct=params.get("min_distinct", 1),
                        max_distinct=params.get("max_distinct"),
                    )
        finally:
            drop_df_table(self._conn, table_name)

        overall_passed = all(r.get("status") == "pass" for r in results.values())
        return QualityResult(
            status="pass" if overall_passed else "fail",
            checks=results,
        )


_QUALITY_CHECK_RULES = ("volume", "null_rate", "distinct_count")


def _rows_to_frame(rows: list[dict], columns: list[str], df_type: str) -> Any:
    """Build a pandas or polars DataFrame with *columns* from a list of row dicts."""
    if df_type == "polars":
        import polars as pl
        if not rows:
            return pl.DataFrame({c: [] for c in columns})
        # Values of several columns can differ in type; strict=False lets
        # polars pick a common supertype instead of raising.
        try:
            return pl.from_dicts(rows, schema=columns, strict=False, infer_schema_length=None)
        except TypeError:  # polars < 1.0 has no 'strict'
            return pl.from_dicts(rows, schema=columns, infer_schema_length=None)
    import pandas as pd
    return pd.DataFrame(rows, columns=columns)


class QualityResult:
    """
    Result of a :meth:`AnofoxConnection.quality_check` run.

    Supports dict-style access for backward compatibility::

        result["status"]  # "pass" or "fail"
        result["checks"]  # {check_name: result_dict}
    """

    def __init__(self, status: str, checks: dict) -> None:
        self.status = status
        self.checks = checks

    def __getitem__(self, key: str) -> Any:
        if key == "status":
            return self.status
        if key == "checks":
            return self.checks
        raise KeyError(key)

    def __repr__(self) -> str:
        check_names = list(self.checks)
        return f"QualityResult(status={self.status!r}, checks={check_names})"

    def _repr_html_(self) -> str:
        _PASS_COLOR = "#27ae60"
        _FAIL_COLOR = "#e74c3c"
        badge_color = _PASS_COLOR if self.status == "pass" else _FAIL_COLOR
        badge_label = "&#10003; PASSED" if self.status == "pass" else "&#10007; FAILED"
        badge = (
            f'<span style="color:{badge_color};font-weight:bold">{badge_label}</span>'
        )
        rows = "".join(
            f"<tr><td>{name}</td>"
            f"<td style='color:{ _PASS_COLOR if r.get('status') == 'pass' else _FAIL_COLOR }'>"
            f"{r.get('status', '?').upper()}</td></tr>"
            for name, r in self.checks.items()
        )
        return (
            f"<div>{badge}"
            f"<table style='margin-top:8px'>"
            f"<tr><th>Check</th><th>Status</th></tr>{rows}"
            f"</table></div>"
        )


class ValidationResult:
    """Result of a schema validation run."""

    def __init__(self, passed: bool, failures: Any) -> None:
        self.passed = passed
        self.failures = failures

    def __repr__(self) -> str:
        n = len(self.failures)
        return f"ValidationResult(passed={self.passed}, failures={n})"

    def _repr_html_(self) -> str:
        _PASS_COLOR = "#27ae60"
        _FAIL_COLOR = "#e74c3c"
        status_color = _PASS_COLOR if self.passed else _FAIL_COLOR
        badge_label = "&#10003; PASSED" if self.passed else "&#10007; FAILED"
        badge = (
            f'<span style="color:{status_color};font-weight:bold">{badge_label}</span>'
        )
        if len(self.failures) == 0:
            failures_html = "<p style='color:#888'>No failures</p>"
        elif hasattr(self.failures, "to_html"):
            failures_html = self.failures.to_html(index=False)
        else:  # polars
            failures_html = self.failures._repr_html_()
        return f"<div>{badge}<br/>{failures_html}</div>"
