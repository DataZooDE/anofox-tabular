"""Parity of the phone module with libphonenumber (issue #133).

The extension implements libphonenumber's parsing, validation and formatting rules over
metadata generated from the ``phonenumbers`` package (scripts/generate_phone_metadata.py).
These tests compare the SQL functions with ``phonenumbers`` itself for the example number of
every type of every supported region, written both in E.164 and in national notation, and
for typo variants of them. ``phonenumbers`` must be the version the metadata was generated
from (see the pin in pyproject.toml and in the generator).
"""

from __future__ import annotations

import os
import random
import subprocess
import sys
from pathlib import Path

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


@pytest.fixture(scope="module")
def duck(ext_path):
    """A plain DuckDB connection with the extension loaded (no anofox package needed, so CI
    can run this against the shipped artifact). ANOFOX_REQUIRE_EXT=1 turns a missing
    extension into a failure instead of a skip."""
    if ext_path is None:
        if os.environ.get("ANOFOX_REQUIRE_EXT") == "1":
            pytest.fail("ANOFOX_REQUIRE_EXT=1 but the extension binary was not found (ANOFOX_EXT_PATH)")
        pytest.skip("Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'.")
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext_path}'")
    con.execute("SET anofox_tab_trace_level = 'warn'")
    yield con
    con.close()


KEYS = ["valid", "possible", "region", "type", "E164", "INTERNATIONAL", "NATIONAL", "RFC3966"]
REPO = Path(__file__).resolve().parents[2]


def expected(number: str, region: str | None) -> dict:
    """What libphonenumber says about the number. An invalid number has no region, type or
    formatted forms (the SQL functions return NULL for them)."""
    result = dict.fromkeys(KEYS)
    try:
        parsed = phonenumbers.parse(number, region)
    except phonenumbers.NumberParseException:
        return {**result, "valid": False, "possible": False}
    valid = phonenumbers.is_valid_number(parsed)
    result.update(valid=valid, possible=phonenumbers.is_possible_number(parsed))
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
    conn.executemany("INSERT INTO phone_parity VALUES (?, ?, ?)", [(i, n, r) for i, (n, r) in enumerate(cases)])
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
    return [dict(zip(KEYS, row)) for row in rows]


def assert_parity(conn, cases: list[tuple[str, str | None]]) -> None:
    got = actual(conn, cases)
    mismatches = [
        f"{number!r} region={region}: expected {want}, got {have}"
        for (number, region), have in zip(cases, got)
        if (want := expected(number, region)) != have
    ]
    assert not mismatches, f"{len(mismatches)} of {len(cases)} differ from libphonenumber:\n" + "\n".join(mismatches[:40])


def test_metadata_is_the_generator_output(tmp_path):
    """The committed metadata is exactly what the generator writes for the pinned version."""
    inc = REPO / "src" / "anofox_phonenumber_metadata_data.inc"
    if not inc.is_file():
        pytest.skip("generated metadata not found")
    out = tmp_path / "metadata.inc"
    subprocess.run(
        [sys.executable, str(REPO / "scripts" / "generate_phone_metadata.py")],
        check=True, capture_output=True, env={**os.environ, "ANOFOX_PHONE_METADATA_OUT": str(out)},
    )
    assert out.read_text() == inc.read_text(), "regenerate with: uv run scripts/generate_phone_metadata.py"


def test_example_numbers_in_e164(duck):
    """Every example number, written with +CC and no region."""
    assert_parity(duck, [(e164, None) for _, e164 in example_numbers()])


def test_example_numbers_in_national_notation(duck):
    """Every example number as written in its country (national format, region given)."""
    cases = []
    for region, e164 in example_numbers():
        national = phonenumbers.format_number(phonenumbers.parse(e164), PhoneNumberFormat.NATIONAL)
        cases.append((national, region))
    assert_parity(duck, cases)


def test_example_numbers_in_international_notation_with_hint(duck):
    """The INTERNATIONAL form with the region as hint, as a CRM export stores it."""
    cases = []
    for region, e164 in example_numbers():
        international = phonenumbers.format_number(phonenumbers.parse(e164), PhoneNumberFormat.INTERNATIONAL)
        cases.append((international, region))
    assert_parity(duck, cases)


def test_typo_variants(duck):
    """A digit dropped or added: valid and possible agree with libphonenumber."""
    cases = []
    for _, e164 in example_numbers():
        cases.append((e164[:-1], None))
        cases.append((e164 + "5", None))
        cases.append((e164 + "55", None))
    assert_parity(duck, cases)


SHARED_CODES = [1, 7, 39, 44, 47, 61, 212, 262, 290, 358, 590, 599]


def test_shared_country_codes_fuzz(duck):
    """Numbers of country codes that several regions share, with their leading digits mutated:
    validity and the region the number is assigned to agree with libphonenumber."""
    rng = random.Random(133)
    cases = []
    for code in SHARED_CODES:
        for region in phonenumbers.COUNTRY_CODE_TO_REGION_CODE[code]:
            for number_type in TYPES:
                example = phonenumbers.example_number_for_type(region, number_type)
                if example is None:
                    continue
                nsn = phonenumbers.national_significant_number(example)
                for _ in range(8):
                    width = rng.randint(1, min(3, len(nsn)))
                    prefix = "".join(rng.choice("0123456789") for _ in range(width))
                    cases.append((f"+{code}{prefix}{nsn[width:]}", None))
    assert len(cases) > 1000
    assert_parity(duck, cases)


def test_is_valid_for_region(duck):
    """isValidNumberForRegion for every example number against every region of its code."""
    cases = []
    for region, e164 in example_numbers():
        code = phonenumbers.parse(e164).country_code
        for candidate in phonenumbers.COUNTRY_CODE_TO_REGION_CODE[code][:4]:
            cases.append((e164, candidate))
    duck.execute("DROP TABLE IF EXISTS phone_parity_region")
    duck.execute("CREATE TABLE phone_parity_region (id INTEGER, n VARCHAR, r VARCHAR)")
    duck.executemany("INSERT INTO phone_parity_region VALUES (?, ?, ?)",
                           [(i, n, r) for i, (n, r) in enumerate(cases)])
    got = [row[0] for row in duck.execute(
        "SELECT phonenumber_is_valid_for_region(n, r) FROM phone_parity_region ORDER BY id").fetchall()]
    duck.execute("DROP TABLE phone_parity_region")
    want = [phonenumbers.is_valid_number_for_region(phonenumbers.parse(n), r) for n, r in cases]
    mismatches = [f"{c}: expected {w}, got {g}" for c, w, g in zip(cases, want, got) if w != g]
    assert not mismatches, "\n".join(mismatches[:40])


def test_local_only_and_trunk_lengths(duck):
    """Lengths only diallable locally are possible but not valid; trunk prefixes are dropped."""
    assert_parity(duck, [("555 0123", "US"), ("253 0000", "US"), ("1234567", "GB"), ("12345", "DE"),
                         ("0171 1234567", "DE"), ("1 650 253 0000", "US"), ("8 495 123 4567", "RU"),
                         ("06 20 123 4567", "HU"), ("8 612 34567", "LT"), ("49 30 1234567", "DE")])
