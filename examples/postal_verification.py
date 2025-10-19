#!/usr/bin/env python3
"""
Postal Verification Example using Anofox Tabular DuckDB Extension

This example demonstrates:
1. Loading the FEBRL address dataset from Parquet
2. Using Anofox Tabular's postal parsing functions
3. Parsing and expanding addresses
4. Analyzing address quality and standardization
5. Handling Australian address formats
"""

import sys
from pathlib import Path

try:
    import duckdb
except ImportError:
    print("Error: duckdb is not installed. Please run: pip install duckdb")
    sys.exit(1)


def get_data_path() -> Path:
    """Get path to the FEBRL address dataset."""
    data_path = Path(__file__).parent / "data" / "febrl_data.parquet"
    if not data_path.exists():
        raise FileNotFoundError(f"Data file not found: {data_path}")
    return data_path


def create_conn():
    """Create a DuckDB connection with unsigned extensions enabled."""
    return duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})


def load_extension(conn):
    """Load Anofox Tabular extension with custom search path if needed."""
    project_root = Path(__file__).parent.parent

    # Try the repository path first (where extensions are deployed)
    repo_path = project_root / "build" / "release" / "repository"
    if repo_path.exists():
        conn.execute(f"SET extension_directory = '{repo_path}'")
        conn.execute("LOAD anofox_tabular")
    else:
        # Try the direct extension path
        ext_path = project_root / "build" / "release" / "extension" / "anofox_tabular"
        if ext_path.exists():
            conn.execute(f"SET extension_directory = '{ext_path.parent}'")
            conn.execute("LOAD anofox_tabular")
        else:
            raise FileNotFoundError(
                f"Anofox Tabular extension not found. "
                f"Please build the extension first: make release\n"
                f"Looked in: {repo_path} and {ext_path}"
            )


def example_1_load_febrl_data():
    """Example 1: Load and inspect the FEBRL address dataset."""
    print("\n" + "=" * 70)
    print("EXAMPLE 1: Loading FEBRL Address Dataset")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    data_path = get_data_path()

    # Load the parquet file
    print(f"\nLoading dataset from: {data_path}")
    conn.execute(f"CREATE TABLE addresses AS SELECT * FROM read_parquet('{data_path}')")

    # Show basic statistics
    stats = conn.execute(
        "SELECT COUNT(*) as total_addresses FROM addresses"
    ).fetchall()[0][0]

    print(f"Dataset contains {stats:,} addresses")

    # Show sample addresses
    print("\nFirst 5 addresses from FEBRL dataset:")
    result = conn.execute(
        """
        SELECT
            row_number() over () as id,
            address_1,
            address_2,
            street_number,
            postcode,
            state
        FROM addresses
        LIMIT 5
        """
    ).fetchall()

    for row in result:
        idx, addr1, addr2, street_num, postcode, state = row
        print(f"\n  Address {idx}:")
        print(f"    Street: {street_num} {addr1}")
        if addr2:
            print(f"    Suburb: {addr2}")
        print(f"    Postcode: {postcode}")
        print(f"    State: {state}")

    conn.close()


def example_2_parse_addresses():
    """Example 2: Parse individual addresses using libpostal."""
    print("\n" + "=" * 70)
    print("EXAMPLE 2: Parse Addresses with libpostal")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Check postal status
    status = conn.execute("SELECT * FROM anofox_postal_status()").fetchall()
    print("\nPostal library status:")
    if status:
        initialized, data_present, data_dir = status[0]
        print(f"  Initialized: {initialized}")
        print(f"  Data present: {data_present}")
        print(f"  Data directory: {data_dir}")

        # Load libpostal data if not already present
        if not data_present:
            print("\n  Loading libpostal data (this may take a moment)...")
            try:
                success = conn.execute("SELECT anofox_postal_load_data()").fetchall()[0][0]
                if success:
                    print("  ✓ Libpostal data loaded successfully")
                else:
                    print("  ✗ Failed to load libpostal data")
                    print("  Skipping parsing examples")
                    conn.close()
                    return
            except Exception as e:
                print(f"  ✗ Error loading data: {e}")
                print("  Skipping parsing examples")
                conn.close()
                return

    # Test some addresses
    addresses = [
        "620 bolger place the burren nsw 4726",
        "3 waite street qld 2285",
        "18 mackennal street little scrubby vic 2770",
    ]

    print("\nParsing sample addresses:")
    for address in addresses:
        try:
            result = conn.execute(
                f"""
                SELECT
                    result.house_number,
                    result.road,
                    result.city,
                    result.state,
                    result.postcode,
                    result.country
                FROM (
                    SELECT anofox_postal_parse_address('{address}') as result
                )
                """
            ).fetchall()

            if result:
                house, road, city, state, postcode, country = result[0]
                print(f"\n  Input: {address}")
                print(f"    House #:  {house}")
                print(f"    Road:     {road}")
                print(f"    City:     {city}")
                print(f"    State:    {state}")
                print(f"    Postcode: {postcode}")
                print(f"    Country:  {country}")
        except Exception as e:
            print(f"\n  Input: {address}")
            print(f"    Error: {e}")

    conn.close()


def example_3_extract_and_parse_from_table():
    """Example 3: Extract and parse addresses from the FEBRL dataset."""
    print("\n" + "=" * 70)
    print("EXAMPLE 3: Extract and Parse Addresses from Dataset")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Ensure libpostal data is loaded
    status = conn.execute("SELECT * FROM anofox_postal_status()").fetchall()
    if status:
        _, data_present, _ = status[0]
        if not data_present:
            print("\nLoading libpostal data...")
            try:
                success = conn.execute("SELECT anofox_postal_load_data()").fetchall()[0][0]
                if not success:
                    print("✗ Failed to load libpostal data. Skipping this example.")
                    conn.close()
                    return
            except Exception as e:
                print(f"✗ Error loading data: {e}. Skipping this example.")
                conn.close()
                return

    data_path = get_data_path()
    conn.execute(f"CREATE TABLE addresses AS SELECT * FROM read_parquet('{data_path}')")

    print("\nCombining address components and parsing first 5 records:")

    result = conn.execute(
        """
        SELECT
            row_number() over () as id,
            COALESCE(address_1, '') ||
            CASE WHEN address_2 IS NOT NULL THEN ' ' || address_2 ELSE '' END ||
            ' ' || COALESCE(street_number, '') ||
            ' ' || COALESCE(postcode, '') ||
            ' ' || COALESCE(state, '') as full_address,
            result.house_number,
            result.road,
            result.city,
            result.state,
            result.postcode
        FROM (
            SELECT
                street_number,
                address_1,
                address_2,
                postcode,
                state,
                anofox_postal_parse_address(
                    COALESCE(address_1, '') ||
                    CASE WHEN address_2 IS NOT NULL THEN ' ' || address_2 ELSE '' END ||
                    ' ' || COALESCE(street_number, '') ||
                    ' ' || COALESCE(postcode, '') ||
                    ' ' || COALESCE(state, '')
                ) as result
            FROM addresses
        )
        LIMIT 5
        """
    ).fetchall()

    for row in result:
        idx, full_addr, house, road, city, state, postcode = row
        print(f"\n  Record {idx}:")
        print(f"    Original: {full_addr}")
        print(f"    Parsed components:")
        print(f"      House #:  {house}")
        print(f"      Road:     {road}")
        print(f"      City:     {city}")
        print(f"      State:    {state}")
        print(f"      Postcode: {postcode}")

    conn.close()


def example_4_address_expansion():
    """Example 4: Generate address variants using expansion."""
    print("\n" + "=" * 70)
    print("EXAMPLE 4: Address Expansion and Normalization")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Ensure libpostal data is loaded
    status = conn.execute("SELECT * FROM anofox_postal_status()").fetchall()
    if status:
        _, data_present, _ = status[0]
        if not data_present:
            print("\nLoading libpostal data...")
            try:
                success = conn.execute("SELECT anofox_postal_load_data()").fetchall()[0][0]
                if not success:
                    print("✗ Failed to load libpostal data. Skipping this example.")
                    conn.close()
                    return
            except Exception as e:
                print(f"✗ Error loading data: {e}. Skipping this example.")
                conn.close()
                return

    # Test some addresses
    addresses = [
        "620 bolger place the burren",
        "3 waite street",
        "18 mackennal street little scrubby",
    ]

    print("\nGenerating address variants:")
    for address in addresses:
        try:
            result = conn.execute(
                f"""
                SELECT
                    UNNEST(anofox_postal_expand_address('{address}')) as variant
                LIMIT 5
                """
            ).fetchall()

            if result:
                print(f"\n  Original: {address}")
                print(f"  Variants:")
                for i, (variant,) in enumerate(result, 1):
                    print(f"    {i}. {variant}")
        except Exception as e:
            print(f"\n  Original: {address}")
            print(f"  Error: {e}")

    conn.close()


def example_5_address_quality_analysis():
    """Example 5: Analyze address quality and completeness."""
    print("\n" + "=" * 70)
    print("EXAMPLE 5: Address Quality and Completeness Analysis")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    data_path = get_data_path()
    conn.execute(f"CREATE TABLE addresses AS SELECT * FROM read_parquet('{data_path}')")

    print("\nAddress completeness metrics:")

    # Analyze data completeness
    completeness = conn.execute(
        """
        SELECT
            COUNT(*) as total_records,
            COUNT(CASE WHEN address_1 IS NOT NULL THEN 1 END) as has_street,
            COUNT(CASE WHEN street_number IS NOT NULL THEN 1 END) as has_street_number,
            COUNT(CASE WHEN postcode IS NOT NULL THEN 1 END) as has_postcode,
            COUNT(CASE WHEN state IS NOT NULL THEN 1 END) as has_state,
            COUNT(CASE WHEN address_2 IS NOT NULL THEN 1 END) as has_suburb
        FROM addresses
        """
    ).fetchall()[0]

    total = completeness[0]
    print(f"\n  Total records: {total}")
    print(f"  Street name: {completeness[1]} ({100*completeness[1]/total:.1f}%)")
    print(f"  Street number: {completeness[2]} ({100*completeness[2]/total:.1f}%)")
    print(f"  Postcode: {completeness[3]} ({100*completeness[3]/total:.1f}%)")
    print(f"  State: {completeness[4]} ({100*completeness[4]/total:.1f}%)")
    print(f"  Suburb: {completeness[5]} ({100*completeness[5]/total:.1f}%)")

    # Show sample records with missing fields
    print("\nSample records with missing fields:")
    missing = conn.execute(
        """
        WITH completeness AS (
            SELECT
                *,
                (address_1 IS NOT NULL)::INT +
                (street_number IS NOT NULL)::INT +
                (postcode IS NOT NULL)::INT +
                (state IS NOT NULL)::INT as complete_fields
            FROM addresses
        )
        SELECT
            row_number() over () as id,
            address_1,
            street_number,
            postcode,
            state,
            complete_fields
        FROM completeness
        WHERE complete_fields < 4
        LIMIT 5
        """
    ).fetchall()

    for idx, addr1, street_num, postcode, state, complete in missing:
        print(f"\n  Record {idx} (complete_fields: {complete}/4):")
        print(f"    Street: {street_num} {addr1}")
        print(f"    Postcode: {postcode}")
        print(f"    State: {state}")

    conn.close()


def main():
    """Run all examples."""
    print("\n" + "=" * 70)
    print("ANOFOX TABULAR POSTAL VERIFICATION EXAMPLES")
    print("=" * 70)

    try:
        example_1_load_febrl_data()
        example_2_parse_addresses()
        example_3_extract_and_parse_from_table()
        example_4_address_expansion()
        example_5_address_quality_analysis()

        print("\n" + "=" * 70)
        print("✓ All examples completed successfully!")
        print("=" * 70 + "\n")

    except Exception as e:
        print(f"\n✗ Error: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
