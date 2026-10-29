"""
Missing values (None, pd.NA, NaN) behave like SQL NULL in every wrapper.

A single missing value gives None, as the SQL function gives NULL. A DataFrame
column that holds only missing values (a float64 column of NaN, as pandas
reads an empty CSV column) gives missing results instead of a binder error.
Integer column names (pandas allows them) work in the quality checks.
"""

import io
import sys
from decimal import Decimal
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

pd = pytest.importorskip("pandas")

MISSING = [None, pd.NA, float("nan")]
MISSING_IDS = ["None", "pd.NA", "NaN"]

#: (module, function, extra positional arguments) of every scalar wrapper whose
#: first argument is the value to check.
SCALAR_WRAPPERS = [
    ("validate", "email_is_valid", ()),
    ("validate", "email_validate", ()),
    ("validate", "phone_is_valid", ()),
    ("validate", "phone_parse", ()),
    ("validate", "phone_format", ()),
    ("validate", "phone_region", ()),
    ("vat", "make_vat", ()),
    ("vat", "vat_split", ()),
    ("vat", "vat_is_valid", ()),
    ("vat", "vat_validate", ()),
    ("vat", "vat_is_valid_syntax", ()),
    ("vat", "vat_exists", ()),
    ("vat", "vat_normalize", ()),
    ("vat", "vat_format", ()),
    ("vat", "is_valid_vat_country", ()),
    ("vat", "vat_is_eu_member", ()),
    ("vat", "vat_country_name", ()),
    ("pii", "pii_detect", ()),
    ("pii", "pii_mask", ()),
    ("pii", "pii_mask_value", ("NAME",)),
    ("pii", "pii_contains", ()),
    ("money", "make_money", ("EUR",)),
    ("money", "money_from_cents", ("EUR",)),
    ("money", "is_valid_currency", ()),
    ("money", "currency_symbol", ()),
    ("money", "currency_name", ()),
    ("money", "money_amount", ()),
    ("money", "money_currency", ()),
    ("money", "money_format", ()),
    ("money", "money_is_positive", ()),
    ("money", "money_is_negative", ()),
    ("money", "money_is_zero", ()),
    ("money", "money_abs", ()),
]


def _func(module, name):
    import importlib
    return getattr(importlib.import_module(f"anofox.{module}"), name)


@pytest.mark.parametrize("missing", MISSING, ids=MISSING_IDS)
@pytest.mark.parametrize("module,name,extra", SCALAR_WRAPPERS, ids=[w[1] for w in SCALAR_WRAPPERS])
def test_single_missing_value_gives_none(conn, module, name, extra, missing):
    assert _func(module, name)(conn, missing, *extra) is None


class TestMoneyWithAMissingOperand:
    EUR = {"amount": Decimal("12.500"), "currency": "EUR"}

    def test_binary_functions(self, conn):
        from anofox import money
        assert money.money_add(conn, self.EUR, None) is None
        assert money.money_subtract(conn, None, self.EUR) is None
        assert money.money_same_currency(conn, self.EUR, None) is None

    def test_missing_factor_or_bound(self, conn):
        from anofox import money
        assert money.money_multiply(conn, self.EUR, float("nan")) is None
        assert money.money_in_range(conn, self.EUR, None, 20) is None

    def test_present_values_still_work(self, conn):
        from anofox import money
        assert money.money_add(conn, self.EUR, self.EUR)["amount"] == Decimal("25.000")
        assert money.make_money(conn, 12.5, "eur") == self.EUR

    def test_infinite_amount_still_raises(self, conn):
        from anofox import money
        with pytest.raises(Exception, match="finite"):
            money.make_money(conn, float("inf"), "EUR")


class TestReplaceStillNeedsADataFrame:
    def test_pii_mask_replace_with_missing_value(self, conn):
        from anofox import pii
        with pytest.raises(ValueError, match="replace=True"):
            pii.pii_mask(conn, pd.NA, replace=True)


def _leads_from_csv():
    """Web-form leads; nobody filled in the optional email, phone and VAT fields."""
    csv = io.StringIO(
        "lead_id,company,email,phone,vat_id,country\n"
        "L-1001,Brandt Logistik GmbH,,,,DE\n"
        "L-1002,Nordlicht Media AG,,,,DE\n"
        "L-1003,Oakridge Supplies Ltd,,,,GB\n"
    )
    return pd.read_csv(csv)


#: (module, function, keyword arguments, column) of the row-wise string wrappers.
ROW_WISE_TEXT = [
    ("validate", "email_is_valid", {}, "email"),
    ("validate", "email_validate", {}, "email"),
    ("validate", "phone_is_valid", {"region_column": "country"}, "phone"),
    ("validate", "phone_parse", {}, "phone"),
    ("validate", "phone_format", {}, "phone"),
    ("validate", "phone_region", {}, "phone"),
    ("vat", "vat_is_valid", {}, "vat_id"),
    ("vat", "vat_validate", {}, "vat_id"),
    ("vat", "vat_normalize", {}, "vat_id"),
    ("vat", "vat_format", {}, "vat_id"),
    ("vat", "make_vat", {}, "vat_id"),
    ("pii", "pii_detect", {"types": ["EMAIL"]}, "email"),
    ("pii", "pii_mask", {"types": ["EMAIL"]}, "email"),
]


class TestAllNanColumn:
    def test_read_csv_gives_float_columns(self):
        leads = _leads_from_csv()
        assert str(leads["email"].dtype) == "float64"

    @pytest.mark.parametrize("module,name,kwargs,column", ROW_WISE_TEXT,
                             ids=[w[1] for w in ROW_WISE_TEXT])
    def test_pandas_all_nan_column_gives_missing_results(self, conn, module, name, kwargs, column):
        leads = _leads_from_csv()
        out = _func(module, name)(conn, leads, column=column, **kwargs)
        assert out[name].isna().all()
        assert out.index.tolist() == leads.index.tolist()
        assert out[column].isna().all()  # the input column itself is unchanged
        assert str(out[column].dtype) == "float64"

    @pytest.mark.parametrize("values", [[None, None, None], [float("nan")] * 3],
                             ids=["null", "nan"])
    def test_polars_all_missing_column(self, conn, values):
        pl = pytest.importorskip("polars")
        from anofox import validate, vat
        leads = pl.DataFrame({"lead_id": ["L-1001", "L-1002", "L-1003"], "email": values})
        out = validate.email_is_valid(conn, leads, column="email")
        assert out["email_is_valid"].to_list() == [None, None, None]
        out = vat.vat_is_valid(conn, leads.rename({"email": "vat_id"}), column="vat_id")
        assert out["vat_is_valid"].to_list() == [None, None, None]

    def test_money_all_nan_currency_column(self, conn):
        from anofox import money
        invoices = pd.DataFrame({"invoice": ["INV-1", "INV-2"], "total": [120.0, 80.5],
                                 "currency": [float("nan"), float("nan")]})
        out = money.make_money(conn, invoices, column="total", currency_column="currency")
        assert out["make_money"].isna().all()

    def test_partly_missing_column_still_validates(self, conn):
        from anofox import validate
        leads = _leads_from_csv()
        leads["email"] = ["ops@brandt-logistik.de", None, "not-an-email"]
        out = validate.email_is_valid(conn, leads, column="email")
        assert out["email_is_valid"].tolist()[0] is True
        assert pd.isna(out["email_is_valid"].iloc[1])
        assert out["email_is_valid"].tolist()[2] is False


class TestIntegerColumnNames:
    """pandas allows int column labels, e.g. from read_csv(header=None)."""

    @pytest.fixture
    def orders(self):
        csv = io.StringIO(
            "SO-1001,2026-10-01,149.90,DE\n"
            "SO-1002,2026-10-01,,DE\n"
            "SO-1003,2026-10-02,89.00,AT\n"
            "SO-1003,2026-10-02,89.00,AT\n"
        )
        return pd.read_csv(csv, header=None)

    def test_null_rate(self, conn, orders):
        from anofox import quality
        result = quality.null_rate(conn, orders, 2, max_null_rate=0.5)
        assert result["status"] == "pass"
        assert result["null_count"] == 1
        assert result["column"] == "2"

    def test_list_of_int_columns(self, conn, orders):
        from anofox import quality
        result = quality.duplicate_count(conn, orders, [0, 1], max_duplicates=0)
        assert result["status"] == "fail"
        assert result["columns"] == "0,1"

    def test_other_checks(self, conn, orders):
        from anofox import quality
        assert quality.values_in_set(conn, orders, 3, ["DE", "AT"])["status"] == "pass"
        assert quality.distinct_count(conn, orders, 0, min_distinct=3)["status"] == "pass"
        assert quality.schema_check(conn, orders, [0, 2])["status"] == "pass"


class TestIntegerColumnLabelsOutsideQuality:
    """Integer column labels work in the anomaly, profile, diff and PII wrappers too."""

    @pytest.fixture
    def readings(self):
        # A sensor export without a header: 0 = sensor id, 1 = temperature, 2 = humidity.
        rows = [f"TMP-{100 + i},{21.0 + (i % 5) * 0.3:.1f},{45 + i % 4}" for i in range(39)]
        rows.append("TMP-139,58.4,12")
        return pd.read_csv(io.StringIO("\n".join(rows) + "\n"), header=None)

    def test_isolation_forest_single_column(self, conn, readings):
        from anofox import anomaly
        out = anomaly.isolation_forest(conn, readings, 1, output_mode="scores",
                                       id_column=0, seed=42, contamination=0.05)
        assert out.columns.tolist()[:2] == ["row_id", "0"]
        assert out.loc[out["is_anomaly"], "0"].tolist() == ["TMP-139"]

    @pytest.mark.parametrize("function", ["isolation_forest_mv", "dbscan_mv", "outlier_tree"])
    def test_multivariate_int_labels(self, conn, readings, function):
        from anofox import anomaly
        kwargs = {"seed": 42} if function == "isolation_forest_mv" else {}
        result = getattr(anomaly, function)(conn, readings, [1, 2], **kwargs)
        assert isinstance(result, dict)

    def test_dbscan_single_column(self, conn, readings):
        from anofox import anomaly
        result = anomaly.dbscan(conn, readings, 1, eps=0.5, min_pts=3)
        assert result["noise_count"] == 1

    def test_all_none_int_column_names_the_column(self, conn, readings):
        from anofox import anomaly
        readings[3] = None
        with pytest.raises(ValueError, match="column '3' has no non-null values"):
            anomaly.isolation_forest(conn, readings, 3)

    def test_profile_int_labels(self, conn, readings):
        from anofox import profile
        out = profile.profile_table(conn, readings, columns=[1, 2])
        assert out["column_name"].tolist() == ["1", "2"]
        corr = profile.profile_correlations(conn, readings, columns=[1, 2])
        assert len(corr) == 1

    def test_diff_int_labels(self, conn, readings):
        from anofox import diff
        after = readings.copy()
        after.loc[5, 1] = 30.0
        changes = diff.joindiff(conn, readings, after, primary_keys=0, compare_columns=[1])
        assert changes["diff_type"].tolist() == ["changed"]

    def test_pii_scan_int_labels(self, conn):
        from anofox import pii
        notes = pd.DataFrame({0: ["T-1", "T-2"], 1: ["mail ana@example.com", "no contact data"]})
        out = pii.pii_scan_table(conn, notes, columns=[1], types=["EMAIL"])
        assert out["column_name"].unique().tolist() == ["1"]


class TestSingleColumnString:
    """A single column name passed where a list is accepted means [name], not its characters."""

    @pytest.fixture(params=["pandas", "polars"])
    def leads(self, request):
        rows = {
            "lead_id": [f"L-{i}" for i in range(1, 7)],
            "email": ["ana@brandt.de", "bob@kroll.at", None, "eva@meier.ch", "x", "ops@brandt.de"],
            "score": [12.0, 48.5, 33.0, 71.2, 5.0, 64.9],
            "visits": [1, 4, 3, 7, 1, 6],
        }
        if request.param == "pandas":
            return pd.DataFrame(rows)
        import polars as pl
        return pl.DataFrame(rows)

    def test_profile_table(self, conn, leads):
        from anofox import profile
        out = profile.profile_table(conn, leads, columns="email")
        assert list(out["column_name"]) == ["email"]

    def test_profile_correlations(self, conn, leads):
        from anofox import profile
        both = profile.profile_correlations(conn, leads, columns=["score", "visits"])
        assert len(both) == 1
        assert len(profile.profile_correlations(conn, leads, columns="score")) == 0  # no pair

    def test_schema_check(self, conn, leads):
        from anofox import quality
        result = quality.schema_check(conn, leads, "email")
        assert result["status"] == "pass"

    def test_joindiff_compare_columns(self, conn, leads):
        from anofox import diff
        changes = diff.joindiff(conn, leads, leads, primary_keys="lead_id", compare_columns="email")
        assert len(changes) == 0
