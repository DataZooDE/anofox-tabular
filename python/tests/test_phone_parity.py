"""Parity of the phone module with libphonenumber (issue #133).

The extension implements libphonenumber's parsing, validation and formatting rules over
metadata generated from the ``phonenumbers`` package (scripts/generate_phone_metadata.py).
These tests compare the SQL functions with ``phonenumbers`` itself for the example number of
every type of every supported region, written both in E.164 and in national notation, and
for typo variants of them. ``phonenumbers`` must be the version the metadata was generated
from (see the pin in pyproject.toml and in the generator).
"""

from __future__ import annotations

import pytest

phonenumbers = pytest.importorskip("phonenumbers")
from phonenumbers import PhoneNumberFormat, PhoneNumberType  # noqa: E402

TYPES = {
    PhoneNumberType.FIXED_LINE: "fixed_line",
    PhoneNumberType.MOBILE: "mobile",
    PhoneNumberType.TOLL_FREE: "toll_free",
    PhoneNumberType.PREMIUM_RATE: "premium_rate",
    PhoneNumberType.SHARED_COST: "shared_cost",
    PhoneNumberType.VOIP: "voip",
    PhoneNumberType.PERSONAL_NUMBER: "personal_number",
    PhoneNumberType.PAGER: "pager",
    PhoneNumberType.UAN: "uan",
    PhoneNumberType.VOICEMAIL: "voicemail",
}
TYPE_NAMES = {**TYPES, PhoneNumberType.FIXED_LINE_OR_MOBILE: "fixed_line_or_mobile", PhoneNumberType.UNKNOWN: "unknown"}


def example_numbers() -> list[tuple[str, str]]:
    """(region, E.164) of every example number of every supported region and type."""
    cases = []
    for region in sorted(phonenumbers.SUPPORTED_REGIONS):
        for number_type in TYPES:
            example = phonenumbers.example_number_for_type(region, number_type)
            if example is not None:
                cases.append((region, phonenumbers.format_number(example, PhoneNumberFormat.E164)))
    return cases


def expected(number: str, region: str | None) -> dict:
    """What libphonenumber says about the number (None fields when it cannot parse it)."""
    try:
        parsed = phonenumbers.parse(number, region)
    except phonenumbers.NumberParseException:
        return {"valid": False, "possible": False}
    valid = phonenumbers.is_valid_number(parsed)
    result = {"valid": valid, "possible": phonenumbers.is_possible_number(parsed)}
    if valid:
        result.update(
            region=phonenumbers.region_code_for_number(parsed),
            type=TYPE_NAMES[phonenumbers.number_type(parsed)],
            E164=phonenumbers.format_number(parsed, PhoneNumberFormat.E164),
            INTERNATIONAL=phonenumbers.format_number(parsed, PhoneNumberFormat.INTERNATIONAL),
            NATIONAL=phonenumbers.format_number(parsed, PhoneNumberFormat.NATIONAL),
            RFC3966=phonenumbers.format_number(parsed, PhoneNumberFormat.RFC3966),
        )
    return result


def actual(conn, cases: list[tuple[str, str | None]]) -> list[dict]:
    """The extension's answers for (number, region) pairs, in order."""
    conn.execute("DROP TABLE IF EXISTS phone_parity")
    conn.execute("CREATE TABLE phone_parity (id INTEGER, n VARCHAR, r VARCHAR)")
    conn._conn.executemany("INSERT INTO phone_parity VALUES (?, ?, ?)", [(i, n, r) for i, (n, r) in enumerate(cases)])
    rows = conn.execute(
        """
        SELECT phonenumber_is_valid(n, r), phonenumber_is_possible(n, r), phonenumber_region(n, r),
               (phonenumber_parse(n, r)).type, phonenumber_format(n, r, 'E164'),
               phonenumber_format(n, r, 'INTERNATIONAL'), phonenumber_format(n, r, 'NATIONAL'),
               phonenumber_format(n, r, 'RFC3966')
        FROM phone_parity ORDER BY id
        """
    ).fetchall()
    conn.execute("DROP TABLE phone_parity")
    keys = ["valid", "possible", "region", "type", "E164", "INTERNATIONAL", "NATIONAL", "RFC3966"]
    results = []
    for row in rows:
        result = dict(zip(keys, row))
        if not result["valid"]:
            result = {"valid": result["valid"], "possible": result["possible"]}
        results.append(result)
    return results


def assert_parity(conn, cases: list[tuple[str, str | None]]) -> None:
    got = actual(conn, cases)
    mismatches = [
        f"{number!r} region={region}: expected {want}, got {have}"
        for (number, region), have in zip(cases, got)
        if (want := expected(number, region)) != have
    ]
    assert not mismatches, f"{len(mismatches)} of {len(cases)} differ from libphonenumber:\n" + "\n".join(mismatches[:40])


def test_metadata_version_matches_phonenumbers():
    inc = (
        __import__("pathlib").Path(__file__).resolve().parents[2] / "src" / "anofox_phonenumber_metadata_data.inc"
    )
    if not inc.is_file():
        pytest.skip("generated metadata not found")
    assert f'"{phonenumbers.__version__}"' in inc.read_text(), "regenerate the metadata or align the phonenumbers pin"


def test_example_numbers_in_e164(conn):
    """Every example number, written with +CC and no region."""
    assert_parity(conn, [(e164, None) for _, e164 in example_numbers()])


def test_example_numbers_in_national_notation(conn):
    """Every example number as written in its country (national format, region given)."""
    cases = []
    for region, e164 in example_numbers():
        national = phonenumbers.format_number(phonenumbers.parse(e164), PhoneNumberFormat.NATIONAL)
        cases.append((national, region))
    assert_parity(conn, cases)


def test_example_numbers_in_international_notation_with_hint(conn):
    """The INTERNATIONAL form with the region as hint, as a CRM export stores it."""
    cases = []
    for region, e164 in example_numbers():
        international = phonenumbers.format_number(phonenumbers.parse(e164), PhoneNumberFormat.INTERNATIONAL)
        cases.append((international, region))
    assert_parity(conn, cases)


def test_typo_variants(conn):
    """A digit dropped or added: valid and possible agree with libphonenumber."""
    cases = []
    for _, e164 in example_numbers():
        cases.append((e164[:-1], None))
        cases.append((e164 + "5", None))
        cases.append((e164 + "55", None))
    assert_parity(conn, cases)
