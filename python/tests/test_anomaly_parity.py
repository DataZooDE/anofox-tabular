"""
The anomaly detectors give the same result for a pandas and a polars DataFrame.

pandas turns a float NaN into SQL NULL when it is copied into DuckDB; polars
keeps it as a NaN value. The wrappers copy a polars NaN as NULL as well, so a
NaN row is left out by every detector for both libraries, like a NULL row.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")
pl = pytest.importorskip("polars")

NAN = float("nan")


def _shipments():
    """30 parcel shipments; SHP-1012 is far too heavy, two weights were not recorded."""
    weights = [2.0 + (i % 6) * 0.25 for i in range(30)]
    distances = [120.0 + (i % 5) * 15.0 for i in range(30)]
    weights[11] = 48.0
    weights[4] = NAN     # scale offline
    weights[20] = None   # not weighed yet
    distances[7] = NAN
    return {
        "shipment_id": [f"SHP-{1001 + i}" for i in range(30)],
        "weight_kg": weights,
        "distance_km": distances,
    }


def _frames():
    data = _shipments()
    return pd.DataFrame(data), pl.DataFrame(data, strict=False)


def _per_row(result):
    """(shipment_id, is_anomaly) of each output row, independent of the library."""
    if isinstance(result, pl.DataFrame):
        result = result.to_pandas()
    return sorted(zip(result["shipment_id"], result["is_anomaly"].astype(bool)))


DETECTORS = [
    ("isolation_forest", "weight_kg", {"seed": 11, "sample_size": 28}),
    ("isolation_forest_mv", ["weight_kg", "distance_km"], {"seed": 11, "sample_size": 27}),
    ("dbscan", "weight_kg", {"eps": 0.5, "min_pts": 3}),
    ("dbscan_mv", ["weight_kg", "distance_km"], {"eps": 20.0, "min_pts": 3}),
]


@pytest.mark.parametrize("function,columns,kwargs", DETECTORS, ids=[d[0] for d in DETECTORS])
def test_summary_is_the_same_for_pandas_and_polars(conn, function, columns, kwargs):
    from anofox import anomaly
    pandas_df, polars_df = _frames()
    from_pandas = getattr(anomaly, function)(conn, pandas_df, columns, **kwargs)
    from_polars = getattr(anomaly, function)(conn, polars_df, columns, **kwargs)
    assert from_pandas == from_polars
    expected = 28 if isinstance(columns, str) else 27  # NaN and None rows are left out
    assert from_pandas["total_count"] == expected


@pytest.mark.parametrize("function,columns,kwargs", DETECTORS, ids=[d[0] for d in DETECTORS])
def test_per_row_output_is_the_same_for_pandas_and_polars(conn, function, columns, kwargs):
    from anofox import anomaly
    pandas_df, polars_df = _frames()
    run = getattr(anomaly, function)
    from_pandas = run(conn, pandas_df, columns, output_mode="scores", id_column="shipment_id", **kwargs)
    from_polars = run(conn, polars_df, columns, output_mode="scores", id_column="shipment_id", **kwargs)
    assert _per_row(from_pandas) == _per_row(from_polars)
    ids = {shipment for shipment, _ in _per_row(from_polars)}
    assert "SHP-1005" not in ids and "SHP-1021" not in ids   # NaN and None weight
    assert ("SHP-1012", True) in _per_row(from_polars)


def test_outlier_tree_is_the_same_for_pandas_and_polars(conn):
    from anofox import anomaly
    pandas_df, polars_df = _frames()
    options = {"min_size_numeric": 5, "z_outlier": 4.0}
    columns = ["weight_kg", "distance_km"]
    assert (anomaly.outlier_tree(conn, pandas_df, columns, **options)
            == anomaly.outlier_tree(conn, polars_df, columns, **options))
    from_pandas = anomaly.outlier_tree(conn, pandas_df, columns, output_mode="outliers",
                                       id_column="shipment_id", **options)
    from_polars = anomaly.outlier_tree(conn, polars_df, columns, output_mode="outliers",
                                       id_column="shipment_id", **options)
    assert sorted(from_pandas["shipment_id"]) == sorted(from_polars["shipment_id"].to_list())


def test_polars_input_is_not_changed(conn):
    from anofox import anomaly
    _, polars_df = _frames()
    anomaly.dbscan(conn, polars_df, "weight_kg", eps=0.5, min_pts=3)
    assert polars_df["weight_kg"].is_nan().sum() == 1
