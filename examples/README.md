# Anofox Tabular Examples

This directory contains practical examples demonstrating how to use the Anofox Tabular DuckDB extension with Python.

## Quick Start

### 1. Set up the Python environment

Using `uv` (recommended, fast):

```bash
cd examples
uv sync
uv run python email_verification.py
```

Or using `pip`:

```bash
cd examples
pip install -r requirements.txt
python email_verification.py
```

### 2. Build the extension (if not already built)

From the project root:

```bash
make release
```

This creates the extension binary at `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension`.

## Examples

### Email Verification (`email_verification.py`)

A comprehensive example demonstrating email validation capabilities using the fraudulent emails dataset. This example shows:

**Features demonstrated:**
- **Example 1: Basic Email Validation** - Quick regex-based validation
- **Example 2: Detailed Validation** - Structured results with validation stages and reasons
- **Example 3: Loading Datasets** - Reading Parquet files with DuckDB
- **Example 4: Extraction & Validation** - Parsing email addresses from complex formats (e.g., "Name <email@domain>")
- **Example 5: Configuration** - Inspecting email validation settings
- **Example 6: Batch Validation** - Processing multiple emails in SQL

**Dataset:**
- `data/fraudulent_emails.parquet` - ~5000 historical fraudulent/spam emails (Enron corpus subset)
- Contains fields: from, to, subject, date, message_id, reply_to, content_type, return_path

**Run the example:**

```bash
uv run python email_verification.py
```

Or with Python directly:

```bash
python email_verification.py
```

**Sample output:**

```
======================================================================
EXAMPLE 1: Basic Email Validation (Regex Mode)
======================================================================

Validating emails with regex mode:
  user@example.com              → ✓ Valid
  invalid.email@                → ✗ Invalid
  person@domain.co.uk           → ✓ Valid
  test+tag@test.org             → ✓ Valid
  no-domain@invalid             → ✗ Invalid
```

## Using Anofox Tabular with Python & DuckDB

### Loading the extension

```python
import duckdb

# Connect to DuckDB and load Anofox Tabular
conn = duckdb.connect(":memory:")
conn.execute("LOAD anofox_tabular")
```

### Email validation functions

```python
# Simple validation
result = conn.execute(
    "SELECT anofox_email_is_valid('user@example.com', 'regex') as valid"
).fetchall()

# Detailed validation with structured results
result = conn.execute("""
    SELECT anofox_email_validate('user@example.com', 'regex') as validation_result
""").fetchall()

# Check configuration
config = conn.execute("SELECT * FROM anofox_email_config()").fetchall()
```

### Common validation modes

- **`'regex'`** (default) - Fast syntax check using regex pattern
- **`'dns'`** - Verify domain MX records exist
- **`'smtp'`** - Full verification including SMTP validation
- **`'full'`** - Alias for `'smtp'` mode

### Reading Parquet files

```python
# Load data directly from Parquet
conn.execute("""
    CREATE TABLE emails AS
    SELECT * FROM read_parquet('data/fraudulent_emails.parquet')
""")

# Query the data
results = conn.execute("""
    SELECT COUNT(*) as total_emails FROM emails
""").fetchall()
```

### SQL integration

```python
# Validate all emails in a table
results = conn.execute("""
    SELECT
        email,
        anofox_email_is_valid(email, 'regex') as valid,
        anofox_email_validate(email, 'regex') as details
    FROM emails
    WHERE anofox_email_is_valid(email, 'regex') = false
    LIMIT 10
""").fetchall()

# Extract emails from complex formats
results = conn.execute("""
    WITH extracted AS (
        SELECT
            "from" as original,
            CASE
                WHEN "from" LIKE '%<%>%'
                THEN TRIM(SUBSTRING("from",
                    POSITION('<' IN "from") + 1,
                    POSITION('>' IN "from") - POSITION('<' IN "from") - 1))
                ELSE "from"
            END as email
        FROM emails
    )
    SELECT
        original,
        email,
        anofox_email_is_valid(email, 'regex') as is_valid
    FROM extracted
""").fetchall()
```

## Environment Setup

### Using `uv` (recommended)

```bash
# Install uv if you don't have it
pip install uv

# Navigate to examples directory
cd examples

# Sync dependencies
uv sync

# Run scripts
uv run python email_verification.py
```

### Using `pip` and `requirements.txt`

```bash
cd examples
pip install -r requirements.txt
python email_verification.py
```

### Manual setup

```bash
cd examples
pip install duckdb>=1.0.0 pyarrow>=13.0.0 polars>=0.19.0
python email_verification.py
```

## Architecture Overview

The examples use the following architecture:

```
┌─────────────────────────────────────────┐
│  Python Script (email_verification.py)  │
├─────────────────────────────────────────┤
│  DuckDB Python API                      │
│  (SQL execution & data processing)      │
├─────────────────────────────────────────┤
│  Anofox Tabular DuckDB Extension        │
│  (Email validation functions)           │
├─────────────────────────────────────────┤
│  External Libraries                     │
│  - libphonenumber (phone parsing)       │
│  - libpostal (address parsing)          │
│  - c-ares (DNS resolution)              │
└─────────────────────────────────────────┘
```

## Data Sources

### Fraudulent Emails Dataset

The `fraudulent_emails.parquet` file contains historical spam/phishing emails from the Enron corpus. It includes:

- **5000+ emails** with realistic patterns
- **Headers**: from, to, subject, date, message_id, reply_to, content_type, return_path
- **Use cases**: Email validation testing, spam detection, regex pattern validation

**Schema:**

```
from            VARCHAR  - Sender with optional display name
to              VARCHAR  - Recipient address
subject         VARCHAR  - Email subject line
date            VARCHAR  - Email date
date_string     VARCHAR  - Date as string
message_id      VARCHAR  - Message identifier
reply_to        VARCHAR  - Reply-to address
content_type    VARCHAR  - MIME content type
return_path     VARCHAR  - Return path for bounces
```

## Performance Tips

1. **Batch processing**: Use SQL to process multiple emails at once
2. **Connection reuse**: Keep the DuckDB connection open across multiple queries
3. **Mode selection**: Use 'regex' for speed, 'dns' for verification, 'smtp' for full validation
4. **Query optimization**: Filter early in the WHERE clause

```python
# ✓ Good: Validate in SQL
results = conn.execute("""
    SELECT * FROM emails
    WHERE anofox_email_is_valid(email, 'regex')
""").fetchall()

# ✗ Slow: Validate in Python loop
for row in conn.execute("SELECT * FROM emails").fetchall():
    if validate_in_python(row.email):
        # process...
```

## Troubleshooting

### Extension not found

**Issue**: "Failed to load extension 'anofox_tabular'"

**Solution**: Build the extension first from the project root:
```bash
cd ..
make release
```

### Data file not found

**Issue**: "Data file not found: .../fraudulent_emails.parquet"

**Solution**: Ensure you're running the script from the `examples` directory:
```bash
cd examples
uv run python email_verification.py
```

### DuckDB import error

**Issue**: "ModuleNotFoundError: No module named 'duckdb'"

**Solution**: Install dependencies:
```bash
uv sync
# or
pip install -r requirements.txt
```

### Connection issues

**Issue**: Port or connection errors

**Solution**: Use in-memory database (default):
```python
conn = duckdb.connect(":memory:")  # In-memory
# or
conn = duckdb.connect("example.db")  # File-based
```

## Further Reading

- [DuckDB Python API Documentation](https://duckdb.org/docs/api/python/overview)
- [Anofox Tabular Documentation](../README.md)
- [Email Validation Best Practices](../README.md#email-verification)

## Contributing

To add more examples:

1. Create a new Python file in this directory
2. Follow the pattern from `email_verification.py`
3. Include docstrings and clear output formatting
4. Add a section to this README describing the example

## License

Same as the main Anofox Tabular project.
