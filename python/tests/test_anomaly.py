"""
Tests for anomaly.py — assert Python API contract (return types, not cluster quality).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pandas = pytest.importorskip("pandas")


@pytest.fixture(scope="module")
def numeric_table(conn):
    """Create a table with numeric data for anomaly tests."""
    conn.execute(
        "CREATE OR REPLACE TABLE _anomaly AS "
        "SELECT unnest(range(1, 51)) AS id, "
        "       unnest(range(1, 51))::DOUBLE AS x, "
        "       (unnest(range(1, 51)) * 2)::DOUBLE AS y"
    )
    yield "_anomaly"
    conn.execute("DROP TABLE IF EXISTS _anomaly")


class TestIsolationForest:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores")
        assert isinstance(result, pd.DataFrame)
        assert len(result) > 0

    def test_dataframe_input_summary(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 31))})
        result = anomaly.isolation_forest(conn, df, "val", output_mode="summary")
        assert isinstance(result, dict)

    def test_dataframe_input_scores(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 31))})
        result = anomaly.isolation_forest(conn, df, "val", output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_raises_for_non_df_non_str(self, conn):
        from anofox import anomaly
        with pytest.raises(TypeError, match="pandas or polars"):
            anomaly.isolation_forest(conn, [1, 2, 3], "col")


class TestIsolationForestMv:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"a": list(range(1, 21)), "b": list(range(1, 21))})
        result = anomaly.isolation_forest_mv(conn, df, ["a", "b"], output_mode="summary")
        assert isinstance(result, dict)


class TestDbscan:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.dbscan(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.dbscan(conn, numeric_table, "x", output_mode="scores")
        assert isinstance(result, pd.DataFrame)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"val": list(range(1, 21))})
        result = anomaly.dbscan(conn, df, "val", output_mode="summary")
        assert isinstance(result, dict)


class TestDbscanMv:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_scores_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, numeric_table, ["x", "y"], output_mode="scores")
        assert isinstance(result, pd.DataFrame)


class TestOutlierTree:
    def test_summary_returns_dict(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, ["x", "y"], output_mode="summary")
        assert isinstance(result, dict)

    def test_outliers_returns_dataframe(self, conn, numeric_table):
        import pandas as pd
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, ["x", "y"], output_mode="outliers")
        assert isinstance(result, pd.DataFrame)

    def test_single_column_string(self, conn, numeric_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, numeric_table, "x", output_mode="summary")
        assert isinstance(result, dict)

    def test_dataframe_input(self, conn):
        import pandas as pd
        from anofox import anomaly
        df = pd.DataFrame({"a": list(range(1, 31)), "b": list(range(1, 31))})
        result = anomaly.outlier_tree(conn, df, ["a", "b"], output_mode="summary")
        assert isinstance(result, dict)


@pytest.fixture(scope="module")
def id_table(conn):
    """Numeric data with a planted outlier and an id that is not the position."""
    conn.execute(
        "CREATE OR REPLACE TABLE _anomaly_ids AS "
        "SELECT 'K' || (1000 - i) AS key, "
        "       CASE WHEN i = 37 THEN 10000.0 ELSE (i % 5)::DOUBLE END AS x, "
        "       CASE WHEN i = 37 THEN 10000.0 ELSE ((i % 5) * 2)::DOUBLE END AS y "
        "FROM range(1, 61) t(i)"
    )
    yield "_anomaly_ids"
    conn.execute("DROP TABLE IF EXISTS _anomaly_ids")


class TestIdColumn:
    def _assert_id_after_row_id(self, result):
        assert list(result.columns[:2]) == ["row_id", "key"]

    def test_isolation_forest(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, id_table, "x", output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert result.sort_values("anomaly_score", ascending=False)["key"].iloc[0] == "K963"

    def test_isolation_forest_mv(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.isolation_forest_mv(conn, id_table, ["x", "y"], output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert result.sort_values("anomaly_score", ascending=False)["key"].iloc[0] == "K963"

    def test_dbscan(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.dbscan(conn, id_table, "x", output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert list(result[result["is_anomaly"]]["key"]) == ["K963"]

    def test_dbscan_mv(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.dbscan_mv(conn, id_table, ["x", "y"], output_mode="scores", id_column="key")
        self._assert_id_after_row_id(result)
        assert list(result[result["is_anomaly"]]["key"]) == ["K963"]

    def test_outlier_tree(self, conn, id_table):
        from anofox import anomaly
        result = anomaly.outlier_tree(conn, id_table, ["x", "y"], output_mode="outliers", id_column="key")
        self._assert_id_after_row_id(result)

    def test_rejected_in_summary_mode(self, conn, id_table):
        import duckdb
        from anofox import anomaly
        with pytest.raises(duckdb.Error, match="id_column requires output_mode"):
            anomaly.isolation_forest(conn, id_table, "x", output_mode="summary", id_column="key")


class TestOutputModes:
    """Every per-row mode returns a DataFrame; unknown modes raise ValueError."""

    @pytest.mark.parametrize("func,arg", [("dbscan", "x"), ("dbscan_mv", ["x", "y"])])
    def test_dbscan_clusters_mode_returns_dataframe(self, conn, numeric_table, func, arg):
        import pandas as pd
        from anofox import anomaly
        result = getattr(anomaly, func)(conn, numeric_table, arg, output_mode="clusters")
        assert isinstance(result, pd.DataFrame)
        assert len(result) == 50
        assert "cluster_id" in result.columns

    @pytest.mark.parametrize("func,arg,mode", [
        ("isolation_forest", "x", "clusters"),
        ("isolation_forest", "x", "outliers"),
        ("isolation_forest_mv", ["x", "y"], "Scores"),
        ("dbscan", "x", "outliers"),
        ("dbscan_mv", ["x", "y"], "summery"),
        ("outlier_tree", ["x", "y"], "scores"),
    ])
    def test_unknown_mode_raises(self, conn, numeric_table, func, arg, mode):
        from anofox import anomaly
        with pytest.raises(ValueError, match="output_mode"):
            getattr(anomaly, func)(conn, numeric_table, arg, output_mode=mode)


@pytest.fixture(scope="module")
def staff_table(conn):
    conn.execute(
        "CREATE OR REPLACE TABLE _staff AS "
        "SELECT i AS staff_id, CASE WHEN i <= 100 THEN 'junior' ELSE 'senior' END AS level, "
        "CASE WHEN i = 7 THEN 92000 WHEN i <= 100 THEN 48000 + (i % 10) * 500 "
        "ELSE 88000 + (i % 10) * 600 END AS salary FROM range(1, 201) t(i)"
    )
    yield "_staff"
    conn.execute("DROP TABLE IF EXISTS _staff")


class TestOutlierTreeDefaults:
    """Python defaults are the SQL defaults: the same call gives the same result."""

    def test_summary_matches_sql_defaults(self, conn, staff_table):
        from anofox import anomaly
        cursor = conn.execute(f"SELECT * FROM outlier_tree('{staff_table}', 'level, salary', 'summary')")
        names = [d[0] for d in cursor.description]
        expected = dict(zip(names, cursor.fetchone()))
        assert anomaly.outlier_tree(conn, staff_table, ["level", "salary"]) == expected

    def test_outliers_match_sql_defaults(self, conn, staff_table):
        from anofox import anomaly
        expected = conn.execute(
            f"SELECT row_id FROM outlier_tree('{staff_table}', 'level, salary', 'outliers') ORDER BY row_id"
        ).fetchall()
        out = anomaly.outlier_tree(conn, staff_table, ["level", "salary"], output_mode="outliers")
        assert sorted(out["row_id"].tolist()) == [r[0] for r in expected]

    def test_explicit_parameter_overrides_default(self, conn, staff_table):
        from anofox import anomaly
        loose = anomaly.outlier_tree(conn, staff_table, ["level", "salary"], z_outlier=1.0,
                                     min_size_numeric=10, max_perc_outliers=0.5)
        strict = anomaly.outlier_tree(conn, staff_table, ["level", "salary"])
        assert loose["outlier_count"] >= strict["outlier_count"]


class TestIsolationForestSeed:
    """seed= makes scores reproducible, as the SQL seed parameter does."""

    def test_same_seed_same_scores(self, conn, numeric_table):
        from anofox import anomaly
        a = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=42)
        b = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=42)
        assert a["anomaly_score"].tolist() == b["anomaly_score"].tolist()

    def test_seed_matches_sql(self, conn, numeric_table):
        from anofox import anomaly
        sql = conn.execute(
            f"SELECT anomaly_score FROM isolation_forest('{numeric_table}', 'x', 100, 256, 0.1, 'scores', 7) "
            "ORDER BY row_id"
        ).fetchall()
        py = anomaly.isolation_forest(conn, numeric_table, "x", output_mode="scores", seed=7)
        assert py["anomaly_score"].tolist() == [r[0] for r in sql]

    def test_mv_same_seed_same_scores(self, conn, numeric_table):
        from anofox import anomaly
        kwargs = dict(output_mode="scores", seed=3, ndim=2)
        a = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], **kwargs)
        b = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], **kwargs)
        assert a["anomaly_score"].tolist() == b["anomaly_score"].tolist()

    def test_mv_ndim_matches_sql(self, conn, numeric_table):
        from anofox import anomaly
        sql = conn.execute(
            f"SELECT anomaly_score FROM isolation_forest_mv('{numeric_table}', 'x, y', 100, 256, 0.1, "
            "'scores', 5, 2) ORDER BY row_id"
        ).fetchall()
        py = anomaly.isolation_forest_mv(conn, numeric_table, ["x", "y"], output_mode="scores", seed=5, ndim=2)
        assert py["anomaly_score"].tolist() == [r[0] for r in sql]


@pytest.fixture(scope="module")
def orders_table(conn):
    conn.execute(
        "CREATE OR REPLACE TABLE _orders AS "
        "SELECT i::DOUBLE AS amount, (i % 7)::DOUBLE AS items FROM range(1, 61) t(i)"
    )
    yield "_orders"
    conn.execute("DROP TABLE IF EXISTS _orders")


class TestColumnsArgument:
    """The multi-column functions take a list or a comma-separated string."""

    @pytest.mark.parametrize("func,extra", [
        ("isolation_forest_mv", {"seed": 1}),
        ("dbscan_mv", {}),
        ("outlier_tree", {}),
    ])
    def test_string_equals_list(self, conn, orders_table, func, extra):
        from anofox import anomaly
        f = getattr(anomaly, func)
        as_list = f(conn, orders_table, ["amount", "items"], **extra)
        as_string = f(conn, orders_table, "amount,items", **extra)
        assert as_string == as_list
        assert as_string.get("n_columns", as_string.get("columns_analyzed")) == 2

    def test_tuple_is_accepted(self, conn, orders_table):
        from anofox import anomaly
        assert anomaly.dbscan_mv(conn, orders_table, ("amount", "items"))["n_columns"] == 2


class TestColumnNamesWithCommas:
    """The SQL functions take a comma-separated column list, so a name with a comma cannot pass."""

    def _readings(self):
        import pandas as pd
        return pd.DataFrame({
            "sensor_id": range(1, 31),
            "reading, c": [20.0 + (i % 4) for i in range(29)] + [80.0],
            "humidity": [40.0 + (i % 3) for i in range(30)],
        })

    @pytest.mark.parametrize("function", ["isolation_forest_mv", "dbscan_mv", "outlier_tree"])
    def test_comma_in_column_name_raises(self, conn, function):
        from anofox import anomaly
        with pytest.raises(ValueError, match=r"'reading, c'.*comma"):
            getattr(anomaly, function)(conn, self._readings(), ["reading, c", "humidity"])

    def test_comma_separated_string_still_works(self, conn):
        from anofox import anomaly
        df = self._readings().rename(columns={"reading, c": "temperature"})
        result = anomaly.isolation_forest_mv(conn, df, "temperature, humidity", seed=7)
        assert isinstance(result, dict)


class TestNanHandling:
    """A polars NaN is a value, not a NULL; each detector treats it its own way (guide 06)."""

    @pytest.fixture
    def amounts(self):
        pl = pytest.importorskip("polars")
        values = [49.9, 50.1, 50.0, 49.8, 50.2, 50.3, 50.0, 49.95,
                  float("nan"), None, float("inf")]
        return pl.DataFrame({"order_id": list(range(101, 112)), "amount": values})

    def test_isolation_forest_skips_nan_like_null_and_flags_inf(self, conn, amounts):
        from anofox import anomaly
        scores = anomaly.isolation_forest(
            conn, amounts, "amount", output_mode="scores", seed=7, id_column="order_id"
        )
        assert sorted(scores["order_id"].to_list()) == list(range(101, 109)) + [111]
        inf_row = scores.filter(scores["order_id"] == 111)
        assert inf_row["is_anomaly"].to_list() == [True]

    def test_dbscan_keeps_nan_as_noise(self, conn, amounts):
        from anofox import anomaly
        clusters = anomaly.dbscan(
            conn, amounts, "amount", output_mode="clusters",
            epsilon=0.5, min_points=3, id_column="order_id",
        )
        noise = clusters.filter(clusters["point_type"] == "NOISE")
        assert sorted(noise["order_id"].to_list()) == [109, 111]

    def test_fill_nan_gives_the_same_rows_to_both(self, conn, amounts):
        from anofox import anomaly
        clean = amounts.fill_nan(None)
        forest = anomaly.isolation_forest(conn, clean, "amount", seed=7)
        clusters = anomaly.dbscan(conn, clean, "amount", epsilon=0.5, min_points=3)
        assert forest["total_count"] == clusters["total_count"] == 9


class TestEmptyAndAllMissingInput:
    """No scored row gives avg_value None (as documented); a column with only
    missing values names that problem instead of 'must be numeric, got NULL'."""

    @staticmethod
    def _payments(amounts):
        return pandas.DataFrame({
            "payment_id": [f"PAY-{7000 + i}" for i in range(len(amounts))],
            "amount": amounts,
            "items": [float(1 + i % 4) for i in range(len(amounts))],
        })

    def test_empty_dataframe_avg_value_is_none(self, conn):
        from anofox import anomaly
        empty = self._payments(pandas.Series([], dtype="float64"))
        result = anomaly.isolation_forest(conn, empty, "amount")
        assert result["total_count"] == 0
        assert result["avg_value"] is None

    def test_all_nan_column_avg_value_is_none(self, conn):
        from anofox import anomaly
        df = self._payments([float("nan")] * 4)
        result = anomaly.isolation_forest(conn, df, "amount")
        assert result["total_count"] == 0
        assert result["avg_value"] is None

    def test_scored_rows_keep_avg_value(self, conn):
        from anofox import anomaly
        df = self._payments([120.0, 80.0, 100.0, 100.0])
        result = anomaly.isolation_forest(conn, df, "amount", seed=1)
        assert result["avg_value"] == pytest.approx(100.0)

    @pytest.mark.parametrize("function, columns", [
        ("isolation_forest", "amount"),
        ("isolation_forest_mv", ["items", "amount"]),
        ("dbscan", "amount"),
        ("dbscan_mv", ["items", "amount"]),
    ])
    def test_all_none_column_raises(self, conn, function, columns):
        from anofox import anomaly
        df = self._payments([None] * 6)
        with pytest.raises(ValueError, match=r"^column 'amount' has no non-null values"):
            getattr(anomaly, function)(conn, df, columns)

    def test_all_none_polars_column_raises(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import anomaly
        df = pl.DataFrame({"payment_id": ["PAY-1", "PAY-2"], "amount": [None, None]})
        with pytest.raises(ValueError, match=r"column 'amount' has no non-null values"):
            anomaly.isolation_forest(conn, df, "amount")


def _staff_frame():
    """200 employees; employee 7 is a junior with a senior salary."""
    staff_ids = list(range(1, 201))
    return pandas.DataFrame({
        "staff_id": [f"E{i:04d}" for i in staff_ids],
        "level": ["junior" if i <= 100 else "senior" for i in staff_ids],
        "salary": [92000 if i == 7 else (48000 + (i % 10) * 500 if i <= 100 else 88000 + (i % 10) * 600)
                   for i in staff_ids],
    })


class TestOutlierTreeConditions:
    """outlier_tree conditions come back as a list of dicts, not as JSON text."""

    _JUNIOR = {"column": "level", "type": "categorical", "operator": "in", "value": None,
               "values": ["junior"]}

    def test_pandas_conditions_are_dicts(self, conn):
        from anofox import anomaly
        out = anomaly.outlier_tree(conn, _staff_frame(), ["level", "salary"], output_mode="outliers",
                                   id_column="staff_id")
        salary = out[out["column_name"] == "salary"]
        assert salary["staff_id"].tolist() == ["E0007"]
        conditions = salary["conditions"].iloc[0]
        assert isinstance(conditions, list)
        assert conditions == [self._JUNIOR]

    def test_numeric_condition_has_value(self, conn):
        from anofox import anomaly
        out = anomaly.outlier_tree(conn, _staff_frame(), ["level", "salary"], output_mode="outliers")
        level = out[out["column_name"] == "level"]
        condition = level["conditions"].iloc[0][0]
        assert condition["column"] == "salary" and condition["type"] == "numeric"
        assert isinstance(condition["value"], float) and condition["values"] is None

    def test_polars_conditions_are_structs(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import anomaly
        out = anomaly.outlier_tree(conn, pl.from_pandas(_staff_frame()), ["level", "salary"],
                                   output_mode="outliers")
        assert out.schema["conditions"] == pl.List(pl.Struct({
            "column": pl.String, "type": pl.String, "operator": pl.String,
            "value": pl.Float64, "values": pl.List(pl.String),
        }))
        salary = out.filter(pl.col("column_name") == "salary")
        assert salary["conditions"].to_list() == [[self._JUNIOR]]

    def test_no_outliers_gives_empty_frame(self, conn):
        from anofox import anomaly
        calm = _staff_frame()
        calm.loc[6, "salary"] = 48500
        out = anomaly.outlier_tree(conn, calm, ["level", "salary"], output_mode="outliers")
        assert len(out) == 0
        assert "conditions" in out.columns


class TestRegisteredViewName:
    """A view registered with conn.register() and passed by name gets a hint to
    pass the DataFrame itself (the detectors read through their own connection)."""

    def test_registered_view_name_suggests_dataframe(self, conn):
        from anofox import anomaly
        conn.register("staff_view", _staff_frame())
        try:
            with pytest.raises(ValueError) as info:
                anomaly.isolation_forest(conn, "staff_view", "salary")
        finally:
            conn.unregister("staff_view")
        message = str(info.value)
        assert "'staff_view' is a temporary view" in message
        assert "pass the DataFrame itself" in message

    def test_pii_scan_of_registered_view(self, conn):
        from anofox import pii
        conn.register("tickets_view", pandas.DataFrame({
            "ticket_id": ["T-501", "T-502"],
            "body": ["Please call me at +49 30 901820", "Order arrived damaged"],
        }))
        try:
            with pytest.raises(ValueError, match=r"'tickets_view' is a temporary view.*pass the DataFrame itself"):
                pii.pii_scan_table(conn, "tickets_view")
        finally:
            conn.unregister("tickets_view")

    def test_temp_table_keeps_sql_error(self, conn):
        import duckdb
        from anofox import anomaly
        conn.execute("CREATE TEMP TABLE staff_tmp AS SELECT 1.0 AS salary").fetchall()
        try:
            with pytest.raises(duckdb.BinderException, match="TEMP tables are not supported"):
                anomaly.isolation_forest(conn, "staff_tmp", "salary")
        finally:
            conn.execute("DROP TABLE staff_tmp").fetchall()

    def test_dataframe_itself_works(self, conn):
        from anofox import anomaly
        result = anomaly.isolation_forest(conn, _staff_frame(), "salary", seed=3)
        assert result["total_count"] == 200
