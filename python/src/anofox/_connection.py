"""
AnofoxConnection — thin wrapper around a DuckDB connection with the
anofox_tabular extension pre-loaded.
"""

from __future__ import annotations

from typing import Any, Optional

import duckdb

from ._loader import AnofoxLoader, _DEFAULT_S3_BASE
from ._sql import bind_params, execute, fetch_one, finish, quote_ident


def _describe_table(conn: Any, name: str) -> tuple[str, list[tuple[str, str]]]:
    """
    Return ``(table_sql, [(column, type), ...])`` for the table *name* refers to.

    *name* is a table name as DuckDB resolves it in a query (the search path
    decides between same-named tables in different schemas), or a qualified
    ``"schema.table"`` / ``"catalog.schema.table"``. A name that is itself a
    table (even one containing dots) wins over the qualified reading.
    """
    def describe(table_sql: str) -> list[tuple[str, str]]:
        return [(row[0], row[1]) for row in conn.execute(f"DESCRIBE {table_sql}").fetchall()]

    table_sql = quote_ident(name)
    try:
        return table_sql, describe(table_sql)
    except duckdb.CatalogException:
        parts = name.split(".")
        if not 2 <= len(parts) <= 3 or not all(parts):
            raise
    table_sql = ".".join(quote_ident(part) for part in parts)
    return table_sql, describe(table_sql)


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
        try:
            # "local", "community" or "download": where the extension came from.
            self._extension_source = loader.load_into(self._conn, local_path=extension_path)
        except BaseException:
            self._conn.close()
            raise

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

        numpy scalars among *parameters* (a value taken from a DataFrame,
        such as ``df["a"].max()``) are bound as the equivalent Python values.
        """
        if parameters is not None:
            return self._conn.execute(query, bind_params(parameters))
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
            A pandas/polars DataFrame, a pyarrow Table or a table name. The name is
            resolved like in a query (the search path picks among same-named
            tables in different schemas); ``"schema.table"`` names a table in
            another schema. An unknown table raises ``duckdb.CatalogException``.
        """
        from ._table import output_df_type, table_inputs

        with table_inputs(self, df_or_table) as (table_name,):
            # DESCRIBE resolves the name like any query does (search path,
            # "schema.table"), so a same-named table in another schema is not
            # mixed in, as it would be with a lookup in information_schema.
            table_sql, cols_result = _describe_table(self._conn, table_name)

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
                output_df_type(df_or_table),
            )

    def validate(
        self,
        df: Any,
        schema: dict,
        include_nulls: bool = False,
        id_column: Optional[str] = None,
    ) -> "ValidationResult":
        """
        Validate a DataFrame against a schema of rules.

        Parameters
        ----------
        df:
            pandas or polars DataFrame.
        schema:
            Mapping of ``{column_name: rule}`` where each rule is an
            :class:`~anofox.validate.EmailRule` or
            :class:`~anofox.validate.PhoneRule`
            (``PhoneRule(region_column="country")`` reads each row's region
            from a column of *df*). Any other rule (including a plain string
            such as ``"email"``) raises :class:`TypeError`. A key (or a
            ``region_column``) that is not a column of *df* raises
            :class:`ValueError`.
        include_nulls:
            ``False`` (default): a missing value (``None``, ``NaN``, ``null``)
            is skipped, as in SQL (the check functions return NULL for it)
            and the row-wise helpers in :mod:`anofox.validate`. ``True``:
            report each missing value as a failure, with the rule name
            followed by ``" missing"`` (e.g. ``"email(regex) missing"``).
        id_column:
            Optional column of *df* holding the business key (e.g.
            ``"customer_id"``). Its value of each failing row is added to the
            failures in a column of that name, directly after ``row_id``. A
            name that is not a column of *df*, or that is one of the failure
            columns (``column``, ``row_id``, ``value``, ``rule``), raises
            :class:`ValueError`.

        Returns
        -------
        ValidationResult
            Contains ``passed`` (bool) and ``failures``, a DataFrame (polars
            for a polars *df*, else pandas) with the
            columns ``column``, ``row_id``, (*id_column*,) ``value`` and ``rule``.
            ``row_id`` identifies the failing row of *df*: its index label for
            a pandas DataFrame, its 0-based row position for a polars DataFrame.
            Failures are ordered by schema key, then row.
        """
        from .validate import EmailRule, PhoneRule
        from ._table import check_columns, column_label
        from ._utils import detect_df_type, registered_view

        df_type = detect_df_type(df)
        id_column = column_label(id_column)
        result_columns = ["column", "row_id", "value", "rule"]
        if id_column is not None:
            if id_column in result_columns:
                raise ValueError(
                    f"id_column {id_column!r} has the name of a failure column "
                    f"({', '.join(result_columns)}); rename it first"
                )
            check_columns(df, [id_column])
            result_columns.insert(2, id_column)
        labels = list(df.index) if df_type == "pandas" else None

        # Resolve every rule before checking any, so a schema with an
        # unsupported rule fails as a whole instead of silently passing it.
        checks = []
        for col, rule in schema.items():
            if isinstance(rule, EmailRule):
                mode = rule.mode
                checks.append((col, "anofox_tab_email_is_valid", mode, f"email({mode})", None))
            elif isinstance(rule, PhoneRule):
                region, region_column = rule.region, rule.region_column
                rule_name = (f"phone(region_column={region_column})" if region_column
                             else f"phone({region or 'default region'})")
                checks.append((col, "anofox_tab_phonenumber_is_valid", region, rule_name, region_column))
            else:
                raise TypeError(
                    f"unsupported rule for column {col!r}: got {rule!r}; "
                    "use one of EmailRule, PhoneRule (from anofox.validate), "
                    "e.g. EmailRule(mode='regex') or PhoneRule(region='DE')"
                )

        # A missing column would surface as a BinderException naming the
        # internal view; report it like the row-wise validators instead.
        check_columns(df, [name for check in checks for name in (check[0], check[4]) if name])

        failure_rows = []
        with registered_view(self._conn, df) as (view, position_column):
            for col, check_sql, param, rule_name, param_column in checks:
                # The second argument is a bound value, or a column of the row.
                argument, params = (quote_ident(param_column), []) if param_column else ("?", [param])
                key_sql = quote_ident(id_column) if id_column is not None else "NULL"
                results = execute(
                    self._conn,
                    f"SELECT {quote_ident(position_column)}, {key_sql}, {quote_ident(col)}, "
                    f"{check_sql}({quote_ident(col)}, {argument}) AS valid "
                    f"FROM {quote_ident(view)} ORDER BY 1",
                    params,
                ).fetchall()
                for position, key, value, valid in results:
                    if value is None:
                        # The check returns NULL for a missing value: not a failure
                        # unless asked for.
                        if not include_nulls:
                            continue
                        rule = f"{rule_name} missing"
                    elif valid:
                        continue
                    else:
                        rule = rule_name
                    failure = {
                        "column": col,
                        "row_id": labels[position] if labels is not None else position,
                        "value": value,
                        "rule": rule,
                    }
                    if id_column is not None:
                        failure[id_column] = key
                    failure_rows.append(failure)

        failures_df = _rows_to_frame(failure_rows, result_columns, df_type)
        return ValidationResult(passed=len(failure_rows) == 0, failures=failures_df)

    def quality_check(self, df: Any, rules: dict) -> "QualityResult":
        """
        Run quality checks on a DataFrame.

        Parameters
        ----------
        df:
            pandas or polars DataFrame, or a pyarrow Table.
        rules:
            Mapping of ``{check_name: params_dict}``. Supported checks and
            their keys:

            - ``"volume"``: ``min_rows`` (default 1), ``max_rows``
            - ``"null_rate"``: ``column`` (required), ``max_null_rate``
              (default 0.0)
            - ``"distinct_count"``: ``column`` (required), ``min_distinct``
              (default 1), ``max_distinct``

            For example ``{"volume": {"min_rows": 100}, "null_rate":
            {"column": "email", "max_null_rate": 0.05}}``. Any other check
            name, a key a check does not know, a missing ``column`` or a value
            that is not a dict raises :class:`ValueError` before a check runs.
            Use :mod:`anofox.quality` for the other checks.

        Returns
        -------
        QualityResult
            ``status`` (``"pass"`` when every check passes, else ``"fail"``)
            and ``checks`` (``{check_name: result_dict}``, the dicts of
            :mod:`anofox.quality`). ``result["status"]`` and
            ``result["checks"]`` work too.
        """
        from ._table import table_inputs
        from ._utils import detect_df_type, is_arrow_table
        from . import quality as q

        if not is_arrow_table(df):
            detect_df_type(df)  # validate type
        unknown = [name for name in rules if name not in _QUALITY_CHECK_RULES]
        if unknown:
            raise ValueError(
                f"unknown quality_check rule(s): {', '.join(map(repr, unknown))}; "
                f"supported: {', '.join(map(repr, _QUALITY_CHECK_RULES))}. "
                "Use anofox.quality for the other checks."
            )
        for check_name, params in rules.items():
            _check_rule_params(check_name, params)
        results = {}
        with table_inputs(self, df) as (table_name,):
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

        overall_passed = all(r.get("status") == "pass" for r in results.values())
        return QualityResult(
            status="pass" if overall_passed else "fail",
            checks=results,
        )


#: The rules of :meth:`AnofoxConnection.quality_check` and the keys each one takes.
_QUALITY_CHECK_KEYS = {
    "volume": ("min_rows", "max_rows"),
    "null_rate": ("column", "max_null_rate"),
    "distinct_count": ("column", "min_distinct", "max_distinct"),
}
_QUALITY_CHECK_RULES = tuple(_QUALITY_CHECK_KEYS)


def _check_rule_params(check_name: str, params: Any) -> None:
    """
    Raise ValueError for a quality_check rule whose parameters are not a dict,
    hold a key the check does not take, or lack a required ``column``.

    A misspelled key (``{"volume": {"min": 10}}``) would otherwise be ignored
    and the check run with its default threshold.
    """
    allowed = _QUALITY_CHECK_KEYS[check_name]
    valid = ", ".join(map(repr, allowed))
    if not isinstance(params, dict):
        raise ValueError(
            f"quality_check rule {check_name!r} takes a dict of parameters "
            f"({valid}), got {params!r}"
        )
    unknown = [key for key in params if key not in allowed]
    if unknown:
        raise ValueError(
            f"unknown key(s) {', '.join(map(repr, unknown))} in quality_check rule "
            f"{check_name!r}; valid keys: {valid}"
        )
    if "column" in allowed and params.get("column") is None:
        raise ValueError(
            f"quality_check rule {check_name!r} needs a 'column', "
            f"e.g. {{{check_name!r}: {{'column': 'email', ...}}}}"
        )


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
