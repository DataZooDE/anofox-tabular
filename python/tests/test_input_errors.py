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
    ("vat", "vat_is_valid"),
    ("vat", "vat_is_valid_syntax"),
    ("vat", "vat_exists"),
    ("vat", "vat_normalize"),
    ("vat", "vat_format"),
    ("vat", "vat_split"),
    ("vat", "make_vat"),
    ("vat", "is_valid_vat_country"),
    ("vat", "vat_is_eu_member"),
    ("vat", "vat_country_name"),
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
def test_none_gives_none(conn, module, name):
    # A missing value is SQL NULL, not a wrong type (see test_missing_inputs.py).
    assert _func(module, name)(conn, None) is None


@pytest.mark.parametrize("module,name", ROW_WISE)
def test_other_objects_are_a_type_error(conn, module, name):
    # A phone number given as a number gets its own hint (it lost its leading 0).
    expected = "phone numbers must be text" if name.startswith("phone_") else "string or a pandas/polars DataFrame"
    with pytest.raises(TypeError, match=expected):
        _func(module, name)(conn, 42)
    with pytest.raises(TypeError, match="string or a pandas/polars DataFrame"):
        _func(module, name)(conn, object())


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


class TestMixedTypeColumn:
    """A pandas object column holding str and int fails in Arrow; the error names it."""

    @pytest.fixture
    def contacts(self):
        return pd.DataFrame({
            "customer": ["Ada Lovelace", "Bob Builder", "Cy Twombly"],
            "phone": ["+49 30 1234567", 4930765432, None],
            "zip": ["10115", "80331", "20095"],
        })

    def _assert_names_column(self, excinfo):
        message = str(excinfo.value)
        assert type(excinfo.value) is TypeError  # not pyarrow's ArrowTypeError
        assert "'phone'" in message
        assert "int" in message and "str" in message
        assert 'astype("string")' in message

    def test_row_wise_function(self, conn, contacts):
        from anofox import validate
        with pytest.raises(TypeError) as excinfo:
            validate.phone_is_valid(conn, contacts, column="phone")
        self._assert_names_column(excinfo)

    def test_table_function(self, conn, contacts):
        from anofox import profile
        with pytest.raises(TypeError) as excinfo:
            profile.profile_summary(conn, contacts)
        self._assert_names_column(excinfo)

    def test_validate_method(self, conn, contacts):
        from anofox.validate import EmailRule
        with pytest.raises(TypeError) as excinfo:
            conn.validate(contacts, {"customer": EmailRule()})
        self._assert_names_column(excinfo)

    def test_str_and_float_column(self, conn):
        from anofox import validate
        orders = pd.DataFrame({"order_ref": ["A-100", 2.5, "A-102"]})
        with pytest.raises(TypeError, match=r"'order_ref'.*float.*str"):
            validate.email_is_valid(conn, orders, column="order_ref")

    def test_fix_from_hint_works(self, conn, contacts):
        from anofox import validate
        contacts["phone"] = contacts["phone"].astype("string")
        result = validate.phone_is_valid(conn, contacts, column="phone")
        assert len(result) == 3
