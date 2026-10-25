"""
Wrong input types get errors that name the problem.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")


ROW_WISE = [
    ("validate", "email_is_valid"),
    ("validate", "email_validate"),
    ("validate", "phone_is_valid"),
    ("validate", "phone_parse"),
    ("validate", "phone_format"),
    ("validate", "phone_region"),
    ("pii", "pii_detect"),
    ("pii", "pii_mask"),
]


def _func(module, name):
    import importlib
    return getattr(importlib.import_module(f"anofox.{module}"), name)


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_series_gets_a_series_hint(conn, module, name):
    series = pd.Series(["a@b.com"], name="email")
    with pytest.raises(TypeError, match=r"Series.*to_frame"):
        _func(module, name)(conn, series)


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_none_is_a_type_error(conn, module, name):
    with pytest.raises(TypeError, match="None"):
        _func(module, name)(conn, None)


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_other_objects_are_a_type_error(conn, module, name):
    with pytest.raises(TypeError, match="string or a pandas/polars DataFrame"):
        _func(module, name)(conn, 42)


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_dataframe_without_column(conn, module, name):
    with pytest.raises(ValueError, match="'column' is required"):
        _func(module, name)(conn, pd.DataFrame({"email": ["a@b.com"]}))


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_missing_column_is_named(conn, module, name):
    with pytest.raises(ValueError, match="'mail'.*email"):
        _func(module, name)(conn, pd.DataFrame({"email": ["a@b.com"]}), column="mail")


class TestPiiMaskDataFrame:
    def test_adds_masked_column(self, conn):
        from anofox import pii
        df = pd.DataFrame({"note": ["mail ana@example.com", "no contact data"]}, index=[4, 2])
        out = pii.pii_mask(conn, df, column="note", strategy="redact")
        assert out.index.tolist() == [4, 2]
        assert out["pii_mask"].tolist() == ["mail [EMAIL]", "no contact data"]
        assert out["note"].tolist() == df["note"].tolist()

    def test_polars(self, conn):
        pl = pytest.importorskip("polars")
        from anofox import pii
        out = pii.pii_mask(conn, pl.DataFrame({"note": ["mail ana@example.com"]}), column="note")
        assert isinstance(out, pl.DataFrame)
        assert out["pii_mask"].to_list() == ["mail [EMAIL]"]


class TestSeriesOnDataFrameMethods:
    """A Series passed where a DataFrame is expected gets the same TypeError hint."""

    @staticmethod
    def _emails():
        return pd.Series(["ana.lopez@example.com", "bad-email"], name="email")

    def test_validate(self, conn):
        from anofox.validate import EmailRule
        with pytest.raises(TypeError, match=r"Series.*to_frame"):
            conn.validate(self._emails(), {"email": EmailRule()})

    def test_profile(self, conn):
        with pytest.raises(TypeError, match=r"Series.*to_frame"):
            conn.profile(self._emails())

    def test_quality_check(self, conn):
        with pytest.raises(TypeError, match=r"Series.*to_frame"):
            conn.quality_check(self._emails(), {"volume": {"min_rows": 1}})

    def test_table_function(self, conn):
        from anofox import quality
        with pytest.raises(TypeError, match=r"Series.*to_frame"):
            quality.volume(conn, self._emails(), min_rows=1)
