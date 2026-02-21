"""
AnofoxConnection — thin wrapper around a DuckDB connection with the
anofox_tabular extension pre-loaded.
"""

from __future__ import annotations

from typing import Any, Optional

import duckdb

from ._loader import AnofoxLoader, _DEFAULT_S3_BASE


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
        self._conn.execute(f"LOAD '{resolved_path}'")

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
        """Execute a SQL statement and return the DuckDB result object."""
        if parameters is not None:
            return self._conn.execute(query, parameters)
        return self._conn.execute(query)

    def sql(self, query: str) -> Any:
        """Execute a SQL statement and return a DuckDB relation."""
        return self._conn.sql(query)

    def register(self, name: str, df: Any) -> None:
        """Register a DataFrame as a named view."""
        self._conn.register(name, df)

    def unregister(self, name: str) -> None:
        """Remove a previously registered view."""
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
        result = self._conn.execute(
            "SELECT count(*) FROM duckdb_functions() WHERE function_name LIKE 'anofox_%'"
        ).fetchone()
        return result is not None and result[0] > 0

    def profile(self, df_or_table: Any) -> Any:
        """
        Profile a DataFrame or named table.

        Returns a pandas DataFrame with per-column quality metrics:
        null_rate, distinct_count, and row volume.

        Parameters
        ----------
        df_or_table:
            Either a pandas/polars DataFrame or a string table name.
        """
        import pandas as pd
        from ._utils import detect_df_type, drop_df_table, register_df_as_table

        if isinstance(df_or_table, str):
            table_name = df_or_table
            registered = False
        else:
            df_type = detect_df_type(df_or_table)
            table_name = register_df_as_table(self._conn, df_or_table)
            registered = True

        try:
            # Get column names and types
            cols_result = self._conn.execute(
                f"SELECT column_name, data_type FROM information_schema.columns "
                f"WHERE table_name = '{table_name}' ORDER BY ordinal_position"
            ).fetchall()

            if not cols_result:
                # Fallback: describe the table
                desc = self._conn.execute(f"DESCRIBE {table_name}").fetchall()
                cols_result = [(row[0], row[1]) for row in desc]

            # Volume (total rows)
            total = self._conn.execute(
                f"SELECT count(*) FROM {table_name}"
            ).fetchone()[0]

            rows = []
            for col_name, col_type in cols_result:
                null_count = self._conn.execute(
                    f"SELECT count(*) FROM {table_name} WHERE \"{col_name}\" IS NULL"
                ).fetchone()[0]
                distinct_count = self._conn.execute(
                    f"SELECT count(DISTINCT \"{col_name}\") FROM {table_name}"
                ).fetchone()[0]

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

            return pd.DataFrame(rows)
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
            :class:`EmailRule`, :class:`PhoneRule`, or callable.

        Returns
        -------
        ValidationResult
            Contains ``passed`` (bool) and ``failures`` (pd.DataFrame).
        """
        from .validate import EmailRule, PhoneRule
        import pandas as pd
        from ._utils import drop_df_table, register_df_as_table

        table_name = register_df_as_table(self._conn, df)

        failure_rows = []
        try:
            for col, rule in schema.items():
                if isinstance(rule, EmailRule):
                    mode = getattr(rule, "mode", "regex")
                    results = self._conn.execute(
                        f"SELECT rowid, \"{col}\", "
                        f"anofox_tab_email_is_valid(\"{col}\", '{mode}') AS valid "
                        f"FROM {table_name}"
                    ).fetchall()
                    for row_id, value, valid in results:
                        if not valid:
                            failure_rows.append({
                                "column": col,
                                "row_id": row_id,
                                "value": value,
                                "rule": f"email({mode})",
                            })
                elif isinstance(rule, PhoneRule):
                    region = getattr(rule, "region", "US")
                    results = self._conn.execute(
                        f"SELECT rowid, \"{col}\", "
                        f"anofox_tab_phonenumber_is_valid(\"{col}\", '{region}') AS valid "
                        f"FROM {table_name}"
                    ).fetchall()
                    for row_id, value, valid in results:
                        if not valid:
                            failure_rows.append({
                                "column": col,
                                "row_id": row_id,
                                "value": value,
                                "rule": f"phone({region})",
                            })
        finally:
            drop_df_table(self._conn, table_name)

        failures_df = pd.DataFrame(failure_rows) if failure_rows else pd.DataFrame(
            columns=["column", "row_id", "value", "rule"]
        )
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
            ``"volume"``, ``"null_rate"``, ``"distinct_count"``.

        Returns
        -------
        dict
            ``{"status": "pass"|"fail", "checks": {check_name: result_dict}}``.
        """
        from ._utils import detect_df_type, drop_df_table, register_df_as_table
        from . import quality as q

        detect_df_type(df)  # validate type
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
        if self.failures.empty:
            failures_html = "<p style='color:#888'>No failures</p>"
        else:
            failures_html = self.failures.to_html(index=False)
        return f"<div>{badge}<br/>{failures_html}</div>"
