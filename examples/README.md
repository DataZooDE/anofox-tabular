# Anofox Tabular Examples

This directory contains practical examples demonstrating how to use the Anofox Tabular DuckDB extension with Python.

## Quick Start

### 1. Get the extension

The scripts load `anofox_tabular` in this order:

1. the file named by the `ANOFOX_EXT_PATH` environment variable;
2. a local build at `../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension`
   (see [Building from Source](../docs/dev/building.md));
3. otherwise `INSTALL anofox_tabular FROM community`, which needs network access.

The `duckdb` Python package must match the DuckDB version the extension was built for
(1.5.6 or 2.0; `uv.lock` pins 1.5.6).

### 2. Run an example

```bash
cd examples
uv sync
uv run python email_verification.py

# with a local build of the extension:
ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  uv run python email_verification.py
```

`postal_verification.py` downloads the libpostal data (~500 MB) on first run.

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
- `data/fraudulent_emails.parquet` - 3,976 historical fraudulent/spam emails
- Contains fields: from, to, subject, date, message_id, reply_to, content_type, return_path

**Run the example:**

```bash
uv run python email_verification.py
```

**Sample output:**

```
======================================================================
EXAMPLE 1: Basic Email Validation (Regex Mode)
======================================================================

Validating emails with regex mode:
  user@example.com               Valid
  invalid.email@                 Invalid
  person@domain.co.uk            Valid
  test+tag@test.org              Valid
  no-domain@invalid              Valid
```

The default regex accepts a single-label domain such as `invalid`; use the `dns` mode to check
that the domain has mail servers.

## Using Anofox Tabular with Python & DuckDB

### Loading the extension

```python
import duckdb

# Connect to DuckDB and load Anofox Tabular
conn = duckdb.connect(":memory:")
conn.execute("INSTALL anofox_tabular FROM community")
conn.execute("LOAD anofox_tabular")
conn.execute("SET anofox_tab_trace_level = 'warn'")  # optional: hide [anofox] [info] lines
```

The `anofox-tabular` package (`pip install anofox-tabular`, imported as `anofox`) does the same
with `anofox.connect()`; see [`python/README.md`](../python/README.md).

### Email validation functions

```python
# Simple validation
result = conn.execute(
    "SELECT email_is_valid('user@example.com', 'regex') as valid"
).fetchall()

# Detailed validation with structured results
result = conn.execute("""
    SELECT email_validate('user@example.com', 'regex') as validation_result
""").fetchall()

# Check configuration
config = conn.execute("SELECT * FROM email_config()").fetchall()
```

### Common validation modes

- **`'regex'`** (default) - Fast syntax check using regex pattern
- **`'dns'`** - Verify domain MX records exist (network: DNS lookup)
- **`'smtp'`** - Full verification including SMTP validation (network: connects to the mail host)
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
# Validate a column: list senders that fail the regex check
results = conn.execute("""
    SELECT
        email,
        email_validate(email, 'regex') as details
    FROM (SELECT "from" AS email FROM emails)
    WHERE email_is_valid(email, 'regex') = false
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
        email_is_valid(email, 'regex') as is_valid
    FROM extracted
""").fetchall()
```

## Environment Setup

### Using `uv` (recommended)

```bash
# Install uv if you don't have it
curl -LsSf https://astral.sh/uv/install.sh | sh

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
pip install "duckdb==1.5.6" "pyarrow>=13.0.0" "polars>=0.19.0"
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

- **3,976 emails** with realistic patterns
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
    WHERE email_is_valid(email, 'regex')
""").fetchall()

# ✗ Slow: Validate in Python loop
for row in conn.execute("SELECT * FROM emails").fetchall():
    if validate_in_python(row.email):
        # process...
```

## Troubleshooting

### Extension not found

**Issue**: "Failed to load extension 'anofox_tabular'" or a version mismatch error

**Solution**: Check that the `duckdb` package version matches the extension (see
[Quick Start](#quick-start)). Without network access, build the extension
(`GEN=ninja make release` from the project root) or point `ANOFOX_EXT_PATH` at a binary.

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
- [Email Validation](../docs/api/01-email.md)

## Contributing

To add more examples:

1. Create a new Python file in this directory
2. Follow the pattern from `email_verification.py`
3. Include docstrings and clear output formatting
4. Add a section to this README describing the example

## License

Same as the main Anofox Tabular project.
