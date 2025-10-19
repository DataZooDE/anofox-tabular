#!/usr/bin/env python3
"""
Email Verification Example using Anofox Tabular DuckDB Extension

This example demonstrates:
1. Loading the fraudulent emails dataset from Parquet
2. Using Anofox Tabular's email validation functions
3. Analyzing validation results and generating reports
4. Extracting email addresses from complex formats
5. Comparing validation methods (regex, DNS, SMTP)
"""

import os
import sys
from pathlib import Path

try:
    import duckdb
except ImportError:
    print("Error: duckdb is not installed. Please run: pip install duckdb")
    sys.exit(1)


def get_data_path() -> Path:
    """Get path to the fraudulent emails dataset."""
    data_path = Path(__file__).parent / "data" / "fraudulent_emails.parquet"
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


def example_1_basic_email_validation():
    """Example 1: Basic email validation using regex mode."""
    print("\n" + "=" * 70)
    print("EXAMPLE 1: Basic Email Validation (Regex Mode)")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Test some emails
    emails = [
        "user@example.com",
        "invalid.email@",
        "person@domain.co.uk",
        "test+tag@test.org",
        "no-domain@invalid",
    ]

    print("\nValidating emails with regex mode:")
    for email in emails:
        # Use the regex validation (fastest, basic syntax check)
        result = conn.execute(
            f"SELECT anofox_email_is_valid('{email}', 'regex') as valid"
        ).fetchall()[0][0]
        status = " Valid" if result else " Invalid"
        print(f"  {email:30} � {status}")

    conn.close()


def example_2_detailed_validation():
    """Example 2: Detailed validation with structured results."""
    print("\n" + "=" * 70)
    print("EXAMPLE 2: Detailed Validation Results")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Sample emails
    test_emails = [
        "alice@company.com",
        "bob@invalid.invalid",
        "charlie@test.org",
    ]

    print("\nDetailed validation results (with reason and stage info):")
    for email in test_emails:
        result = conn.execute(
            f"""
            SELECT
                '{email}' as email,
                result.valid,
                result.stage,
                result.reason
            FROM (
                SELECT anofox_email_validate('{email}', 'regex') as result
            )
            """
        ).fetchall()

        if result:
            email_addr, valid, stage, reason = result[0]
            status = "Valid" if valid else f"Invalid ({reason})"
            print(f"  {email_addr:25} � {status} [Stage: {stage}]")

    conn.close()


def example_3_load_fraudulent_data():
    """Example 3: Load and analyze fraudulent emails dataset."""
    print("\n" + "=" * 70)
    print("EXAMPLE 3: Loading Fraudulent Emails Dataset")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    data_path = get_data_path()

    # Load the parquet file
    print(f"\nLoading dataset from: {data_path}")
    conn.execute(f"CREATE TABLE emails AS SELECT * FROM read_parquet('{data_path}')")

    # Show basic statistics
    stats = conn.execute(
        "SELECT COUNT(*) as total_emails FROM emails"
    ).fetchall()[0][0]

    print(f"Dataset contains {stats:,} emails")

    # Show sample emails
    print("\nFirst 3 fraudulent emails:")
    result = conn.execute(
        """
        SELECT
            row_number() over () as id,
            "from" as sender,
            "to" as recipient,
            subject
        FROM emails
        LIMIT 3
        """
    ).fetchall()

    for row in result:
        idx, sender, recipient, subject = row
        print(f"\n  Email {idx}:")
        print(f"    From:    {sender[:50]}")
        print(f"    To:      {recipient}")
        print(f"    Subject: {subject[:50]}")

    conn.close()


def example_4_extract_and_validate_senders():
    """Example 4: Extract sender emails and validate them."""
    print("\n" + "=" * 70)
    print("EXAMPLE 4: Extract and Validate Sender Emails")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    data_path = get_data_path()
    conn.execute(f"CREATE TABLE emails AS SELECT * FROM read_parquet('{data_path}')")

    print("\nExtracting sender email addresses and validating them...")

    # Extract email from "Name <email@domain>" format
    result = conn.execute(
        """
        SELECT
            "from" as original_from,
            CASE
                WHEN "from" LIKE '%<%>%'
                THEN TRIM(SUBSTRING("from", POSITION('<' IN "from") + 1, POSITION('>' IN "from") - POSITION('<' IN "from") - 1))
                ELSE "from"
            END as extracted_email,
            anofox_email_is_valid(
                CASE
                    WHEN "from" LIKE '%<%>%'
                    THEN TRIM(SUBSTRING("from", POSITION('<' IN "from") + 1, POSITION('>' IN "from") - POSITION('<' IN "from") - 1))
                    ELSE "from"
                END,
                'regex'
            ) as is_valid
        FROM emails
        LIMIT 5
        """
    ).fetchall()

    print("\nSender emails validation results:")
    for original, extracted, valid in result:
        status = "" if valid else ""
        print(f"\n  {status} Original:  {original}")
        print(f"    Extracted: {extracted}")
        print(f"    Valid:     {valid}")

    # Show validation summary
    summary = conn.execute(
        """
        WITH extracted AS (
            SELECT
                CASE
                    WHEN "from" LIKE '%<%>%'
                    THEN TRIM(SUBSTRING("from", POSITION('<' IN "from") + 1, POSITION('>' IN "from") - POSITION('<' IN "from") - 1))
                    ELSE "from"
                END as email
            FROM emails
        )
        SELECT
            COUNT(*) as total_senders,
            SUM(CASE WHEN anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as valid_count,
            SUM(CASE WHEN NOT anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as invalid_count
        FROM extracted
        """
    ).fetchall()[0]

    total, valid, invalid = summary
    print(f"\nValidation Summary:")
    print(f"  Total senders: {total}")
    print(f"  Valid emails:  {valid} ({100*valid//total}%)")
    print(f"  Invalid emails: {invalid} ({100*invalid//total}%)")

    conn.close()


def example_5_email_config():
    """Example 5: Check email validation configuration."""
    print("\n" + "=" * 70)
    print("EXAMPLE 5: Email Validation Configuration")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    print("\nCurrent email validation configuration:")
    result = conn.execute("SELECT * FROM anofox_email_config()").fetchall()

    for key, value in result:
        # Truncate long values for display
        if len(str(value)) > 60:
            value = str(value)[:57] + "..."
        print(f"  {key:35} = {value}")

    conn.close()


def example_6_batch_validation():
    """Example 6: Batch validation of emails with SQL."""
    print("\n" + "=" * 70)
    print("EXAMPLE 6: Batch Validation with SQL")
    print("=" * 70)

    conn = create_conn()
    load_extension(conn)

    # Create a test table
    emails_to_test = [
        (1, 'alice@company.com'),
        (2, 'bob@invalid.invalid'),
        (3, 'charlie@example.org'),
        (4, 'malformed@'),
        (5, 'dave@test.co.uk'),
        (6, 'eve@localhost'),
        (7, 'frank.name+tag@domain.net'),
        (8, 'no-at-sign.email'),
        (9, 'george@valid.com'),
        (10, 'helen@test.invalid')
    ]

    conn.execute("CREATE TABLE test_emails (id INTEGER, email VARCHAR)")
    for email_id, email in emails_to_test:
        conn.execute(f"INSERT INTO test_emails VALUES ({email_id}, '{email}')")

    print("\nValidation results for batch of 10 emails:")
    result = conn.execute("""
        SELECT
            id,
            email,
            anofox_email_is_valid(email, 'regex') as valid_regex,
            CASE
                WHEN anofox_email_is_valid(email, 'regex') THEN ''
                ELSE ''
            END as status
        FROM test_emails
        ORDER BY id
    """).fetchall()

    print(f"\n{'ID':>3} {'Status':>6} {'Email':<35} {'Valid':>5}")
    print("-" * 50)
    for row in result:
        idx, email, valid, status = row
        print(f"{idx:3d} {status:>6} {email:<35} {str(valid):>5}")

    # Summary statistics
    summary = conn.execute("""
        SELECT
            COUNT(*) as total,
            SUM(CASE WHEN anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as valid,
            SUM(CASE WHEN NOT anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as invalid
        FROM test_emails
    """).fetchall()[0]

    total, valid, invalid = summary
    print(f"\nSummary: {valid}/{total} valid ({100*valid//total}%), {invalid}/{total} invalid")

    conn.close()


def main():
    """Run all examples."""
    print("\n" + "=" * 70)
    print("ANOFOX TABULAR EMAIL VERIFICATION EXAMPLES")
    print("=" * 70)

    try:
        example_1_basic_email_validation()
        example_2_detailed_validation()
        example_3_load_fraudulent_data()
        example_4_extract_and_validate_senders()
        example_5_email_config()
        example_6_batch_validation()

        print("\n" + "=" * 70)
        print(" All examples completed successfully!")
        print("=" * 70 + "\n")

    except Exception as e:
        print(f"\n Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
