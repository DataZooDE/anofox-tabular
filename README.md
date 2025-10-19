# Anofox Tabular DuckDB Extension

Anofox Tabular enriches DuckDB with production‑ready validation primitives for customer data. It provides:

- **Email validation** with regex, DNS and SMTP checks, including configurable timeouts and structured results.
- **Postal parsing & expansion** powered by libpostal, plus helpers to manage the required data bundles.
- **Phone number parsing/formatting** backed by libphonenumber for international coverage.
- **Data quality metrics** including volume, null rate, distinct count, z-score, IQR, freshness, schema validation, and DBSCAN clustering.
- **Isolation forest anomaly detection** for both univariate and multivariate outlier detection with configurable sensitivity.
- **DBSCAN clustering** for density-based anomaly detection across univariate and multivariate data.
- **Data diffing** for comparing tables and identifying changes, inspired by Datafold's data-diff tool.

The project builds on DuckDB’s extension template but replaces the placeholder code with a realistic data quality toolkit.

---

## Quick Start

```bash
git clone https://github.com/datazoo/anofox-tabular.git
cd anofox-tabular
git submodule update --init --recursive
make

# launch DuckDB with the extension preloaded
./build/release/duckdb
```

From inside DuckDB:

```sql
LOAD anofox_tabular;
SELECT * FROM anofox_email_config();
```

The build produces three primary artefacts:

- `build/release/duckdb` – DuckDB CLI with the extension statically linked.
- `build/release/test/unittest` – DuckDB’s test runner including the extension.
- `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` – the distributable loadable binary.

### Dependencies

Vcpkg is used to fetch libpostal, libphonenumber, c-ares and spdlog. If you do not have a vcpkg checkout yet:

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

The extension’s `vcpkg.json` pins the required libraries – building the project will install them automatically.

## Feature Overview

### Email Verification
- `anofox_email_is_valid(email [, mode])` → boolean.
- `anofox_email_validate(email [, mode])` → struct report.
- `anofox_email_config()` → table of active configuration.

Validation modes: `regex` (default), `dns`, `smtp`, or `full` (alias for `smtp`). SMTP verification respects per-host and overall timeouts and records a transcript.

### Postal Utilities
- `anofox_postal_parse_address(address)` → struct with house_number, road, city, state, postcode, country.
- `anofox_postal_expand_address(address)` → list of normalised address variants.
- `anofox_postal_load_data()` → downloads and extracts the libpostal assets into the configured directory.
- `anofox_postal_status()` → table describing library status and data directory.

### Phone Number Utilities
- `anofox_phonenumber_parse(number, region_hint)` → struct with validity, country code, national number, inferred region and type.
- `anofox_phonenumber_format(number, region_hint, format)` → formatted string (`E164`, `INTERNATIONAL`, `NATIONAL`, `RFC3966`).
- `anofox_phonenumber_region(number, region_hint)` → ISO region code.
- `anofox_phonenumber_status()` → table reporting libphonenumber initialisation state and default region.

## SQL Function Reference

| Function | Kind | Signature | Notes |
|----------|------|-----------|-------|
| `anofox_email_is_valid` | Scalar | `(VARCHAR email [, VARCHAR mode]) → BOOLEAN` | Quick boolean verdict. |
| `anofox_email_validate` | Scalar | `(VARCHAR email [, VARCHAR mode]) → STRUCT(valid BOOLEAN, stage VARCHAR, reason VARCHAR, mx_hosts LIST<VARCHAR>, smtp_debug STRUCT(transcript LIST<VARCHAR>))` | Detailed validation with MX list and SMTP transcript. |
| `anofox_email_config` | Table | `() → TABLE(key VARCHAR, value VARCHAR)` | Current configuration snapshot. |
| `anofox_postal_parse_address` | Scalar | `(VARCHAR address) → STRUCT(house_number, road, city, state, postcode, country)` | Wraps libpostal parser. |
| `anofox_postal_expand_address` | Scalar | `(VARCHAR address) → LIST<VARCHAR>` | Generates expanded address candidates. |
| `anofox_postal_status` | Table | `() → TABLE(initialized BOOLEAN, data_present BOOLEAN, data_dir VARCHAR)` | Verifies libpostal availability. |
| `anofox_postal_load_data` | Scalar | `() → BOOLEAN` | Downloads/extracts assets; returns true on success. |
| `anofox_phonenumber_parse` | Scalar | `(VARCHAR number, VARCHAR region_hint) → STRUCT(valid BOOLEAN, country_code INTEGER, national_number VARCHAR, region VARCHAR, type VARCHAR)` | Region hint defaults to configured option when NULL. |
| `anofox_phonenumber_format` | Scalar | `(VARCHAR number, VARCHAR region_hint, VARCHAR format) → VARCHAR` | Throws on invalid inputs. |
| `anofox_phonenumber_region` | Scalar | `(VARCHAR number, VARCHAR region_hint) → VARCHAR` | Returns best-effort ISO code. |
| `anofox_phonenumber_status` | Table | `() → TABLE(initialized BOOLEAN, default_region VARCHAR)` | Reports manager state. |
| `anofox_metric_volume` | Table | `(VARCHAR table_name, [BIGINT min_rows], [BIGINT max_rows]) → TABLE(status VARCHAR, row_count BIGINT, min_threshold BIGINT, max_threshold BIGINT, message VARCHAR)` | Validates row count against min/max thresholds. |
| `anofox_metric_null_rate` | Table | `(VARCHAR table_name, VARCHAR column_name, [DOUBLE max_null_rate]) → TABLE(status VARCHAR, null_count BIGINT, total_count BIGINT, null_rate DOUBLE, threshold DOUBLE, message VARCHAR)` | Checks null rate in a column (default threshold 1.0). |
| `anofox_metric_distinct_count` | Table | `(VARCHAR table_name, VARCHAR column_name, [BIGINT min_distinct], [BIGINT max_distinct]) → TABLE(status VARCHAR, distinct_count BIGINT, min_threshold BIGINT, max_threshold BIGINT, message VARCHAR)` | Validates distinct value count. |
| `anofox_metric_schema` | Table | `(VARCHAR table_name, LIST<VARCHAR> required_columns) → TABLE(status VARCHAR, missing_columns LIST<VARCHAR>, message VARCHAR)` | Checks for required columns. |
| `anofox_metric_freshness` | Table | `(VARCHAR table_name, VARCHAR timestamp_column, INTERVAL max_age, [TIMESTAMP reference_time]) → TABLE(status VARCHAR, metric_value TIMESTAMP, threshold TIMESTAMP, age_seconds BIGINT, message VARCHAR)` | Validates data recency. |
| `anofox_metric_zscore` | Table | `(VARCHAR table_name, VARCHAR column_name, [DOUBLE threshold]) → TABLE(status VARCHAR, mean DOUBLE, stddev DOUBLE, outlier_count BIGINT, total_count BIGINT, outlier_rate DOUBLE, threshold DOUBLE, message VARCHAR)` | Detects outliers using z-score (default threshold 3.0). |
| `anofox_metric_iqr` | Table | `(VARCHAR table_name, VARCHAR column_name, [DOUBLE iqr_multiplier]) → TABLE(status VARCHAR, q1 DOUBLE, q3 DOUBLE, iqr DOUBLE, lower_bound DOUBLE, upper_bound DOUBLE, outlier_count BIGINT, message VARCHAR)` | Detects outliers using IQR method (default multiplier 1.5). |
| `anofox_metric_isolation_forest` | Table | `(VARCHAR table_name, VARCHAR column_name, [BIGINT n_trees], [BIGINT sample_size], [DOUBLE contamination], [VARCHAR output_mode]) → TABLE(…)` | Univariate anomaly detection using isolation forest algorithm. |
| `anofox_metric_isolation_forest_multivariate` | Table | `(VARCHAR table_name, VARCHAR column_names, [BIGINT n_trees], [BIGINT sample_size], [DOUBLE contamination], [VARCHAR output_mode]) → TABLE(…)` | Multivariate anomaly detection using isolation forest (comma-separated column list). |
| `anofox_metric_dbscan` | Table | `(VARCHAR table_name, VARCHAR column_name, [DOUBLE eps], [BIGINT min_pts], [VARCHAR output_mode]) → TABLE(…)` | Univariate density-based clustering (DBSCAN) for anomaly detection. |
| `anofox_metric_dbscan_multivariate` | Table | `(VARCHAR table_name, VARCHAR column_names, [DOUBLE eps], [BIGINT min_pts], [VARCHAR output_mode]) → TABLE(…)` | Multivariate DBSCAN clustering (comma-separated column list). |

### Isolation Forest Anomaly Detection

The Isolation Forest algorithm detects anomalies by isolating observations through random splitting. It excels at detecting multivariate outliers and works well with high-dimensional data.

#### Univariate Example

```sql
-- Detect outliers in a single numeric column (summary mode)
SELECT * FROM anofox_metric_isolation_forest(
    'sales_data',           -- table name
    'amount',               -- column to analyze
    100,                    -- n_trees: number of trees (1-500, default 100)
    256,                    -- sample_size: subsample size per tree (1-10000, default 256)
    0.1,                    -- contamination: expected fraction of outliers (0.0-0.5, default 0.1)
    'summary'               -- output_mode: 'summary' or 'scores' (default 'summary')
);

-- Get per-row anomaly scores (scores mode)
SELECT * FROM anofox_metric_isolation_forest(
    'sales_data', 'amount', 100, 256, 0.1, 'scores'
) WHERE is_anomaly = true;
```

#### Multivariate Example

```sql
-- Detect anomalies across multiple columns
SELECT * FROM anofox_metric_isolation_forest_multivariate(
    'customer_events',      -- table name
    'purchase_amount, time_since_signup, page_views',  -- column list (comma-separated)
    100,                    -- n_trees
    256,                    -- sample_size
    0.1,                    -- contamination
    'summary'               -- output_mode
);

-- Get per-row anomaly scores with details
SELECT
    row_id,
    anomaly_score,
    is_anomaly,
    CASE
        WHEN is_anomaly THEN 'Flagged as anomaly'
        ELSE 'Normal'
    END as flag
FROM anofox_metric_isolation_forest_multivariate(
    'customer_events', 'purchase_amount, time_since_signup, page_views',
    100, 256, 0.1, 'scores'
) ORDER BY anomaly_score DESC;
```

#### Output Modes

- **`summary`** (default): Returns single-row aggregate with `status`, `outlier_count`, `total_count`, `n_columns`, `contamination`, `n_trees`, and `message`.
- **`scores`**: Returns per-row output with `row_id`, `anomaly_score` (0.0-1.0), and `is_anomaly` (boolean).

#### Algorithm Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `n_trees` | BIGINT | 1–500 | 100 | Number of isolation trees in the ensemble. Higher values improve accuracy but increase computation time. |
| `sample_size` | BIGINT | 1–10000 | 256 | Number of samples drawn for each tree (subsample size). The algorithm computes max_depth as `ceil(log2(sample_size)) + 1`. |
| `contamination` | DOUBLE | 0.0–0.5 | 0.1 | Expected fraction of anomalies. Used to determine the anomaly threshold via quantile. Higher values flag more points as anomalies. |
| `output_mode` | VARCHAR | `'summary'`, `'scores'` | `'summary'` | Determines the shape of the output (aggregate vs. per-row scores). |

#### Algorithm Details

- Isolation Forest works by recursively partitioning data on random features. Anomalies are easier to isolate (require fewer splits) than normal observations.
- The anomaly score is computed as `2^(-avg_path_length / c)`, where `c` is the expected path length for a random binary search tree. Scores range from 0.0 (inlier) to 1.0 (strong anomaly).
- The algorithm is particularly effective for:
  - High-dimensional data (scales well with feature count)
  - Detecting global and local anomalies
  - Non-parametric anomaly detection (no distributional assumptions)

### DBSCAN Clustering & Anomaly Detection

DBSCAN (Density-Based Spatial Clustering of Applications with Noise) identifies clusters of varying shapes and sizes, and automatically labels sparse regions as noise/outliers. It's particularly effective for detecting local density-based anomalies and doesn't require prior knowledge of the number of clusters.

#### Univariate Example

```sql
-- Detect clusters and noise in a single numeric column (summary mode)
SELECT * FROM anofox_metric_dbscan(
    'transactions',          -- table name
    'amount',                -- column to analyze
    10.0,                    -- eps: neighborhood radius (default 0.5)
    5,                       -- min_pts: min points to form dense region (default 5)
    'summary'                -- output_mode: 'summary' or 'clusters' (default 'summary')
);

-- Get per-row cluster assignments and anomaly scores
SELECT * FROM anofox_metric_dbscan(
    'transactions', 'amount', 10.0, 5, 'clusters'
) WHERE point_type IN ('NOISE', 'BORDER')
ORDER BY anomaly_score DESC;
```

#### Multivariate Example

```sql
-- Detect anomalies in multi-dimensional space
SELECT * FROM anofox_metric_dbscan_multivariate(
    'customer_events',       -- table name
    'purchase_amount, time_since_signup, page_views',  -- columns (comma-separated)
    15.0,                    -- eps: distance threshold
    4,                       -- min_pts
    'summary'                -- output_mode
);

-- Get per-row results with cluster and point type information
SELECT
    row_id,
    cluster_id,
    point_type,
    anomaly_score,
    CASE
        WHEN cluster_id = -1 THEN 'Noise/Outlier'
        WHEN point_type = 'CORE' THEN 'Dense Core'
        WHEN point_type = 'BORDER' THEN 'Boundary'
        ELSE 'Other'
    END as classification
FROM anofox_metric_dbscan_multivariate(
    'customer_events', 'purchase_amount, time_since_signup, page_views',
    15.0, 4, 'clusters'
) ORDER BY anomaly_score DESC;
```

#### Output Modes

- **`summary`** (default): Returns single-row aggregate with `status`, `noise_count`, `total_count`, `cluster_count`, and `message`.
- **`clusters`**: Returns per-row output with `row_id`, `cluster_id` (-1 for noise), `point_type` (CORE/BORDER/NOISE), `anomaly_score` (0.0-1.0), and `is_anomaly`.

#### Algorithm Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `eps` | DOUBLE | 0.5 | Neighborhood radius for density search. Points within this distance are neighbors. Larger values create bigger clusters; smaller values create more noise points. |
| `min_pts` | BIGINT | 5 | Minimum number of points within `eps` distance to form a dense region (core point). |
| `output_mode` | VARCHAR | `'summary'` | Output shape: `'summary'` for aggregate stats or `'clusters'` for per-row results. |

#### Distance Metrics

DBSCAN supports three distance metrics (selected internally for optimal performance):
- **Euclidean**: `sqrt(sum((p1[i] - p2[i])^2))` - standard distance in Cartesian space
- **Manhattan**: `sum(|p1[i] - p2[i]|)` - block/city distance
- **Chebyshev**: `max(|p1[i] - p2[i]|)` - chess king distance

#### Point Classification

DBSCAN categorizes each point into one of four types:

| Type | Definition | Anomaly Score |
|------|-----------|---|
| **CORE** | Has ≥ `min_pts` neighbors within `eps` | Low (0.1-0.3): Dense region |
| **BORDER** | Has < `min_pts` neighbors but is reachable from a core point | Moderate (0.3-0.6): Boundary |
| **NOISE** | Not reachable from any core point; isolated | High (1.0): Clear outlier |
| **UNVISITED** | Not yet assigned in initial pass | N/A |

#### Algorithm Details

- DBSCAN uses density-based connectivity: a point belongs to a cluster if it's close to dense regions, not just to cluster centers.
- It automatically finds natural groupings in the data without requiring the number of clusters in advance.
- Sparse regions (low-density areas) are naturally marked as noise, making it excellent for outlier detection.
- Scales to datasets with varying density levels and non-convex cluster shapes.

## Configuration Options

Set options with `SET …` or persist via DuckDB’s configuration mechanisms.

| Option | Description | Default |
|--------|-------------|---------|
| `anofox_trace_enabled` | Enable/disable tracing output globally. | `true` |
| `anofox_trace_level` | Minimum trace level (`trace/debug/info/warn/error/critical/off`). | `info` |
| `anofox_email_default_validation` | Default mode for `anofox_email_is_valid`. | `regex` |
| `anofox_email_regex_pattern` | ECMAScript regex used in the first stage. | RFC 5322-inspired pattern |
| `anofox_email_dns_timeout_ms` | Per-try DNS timeout (1–5000 ms). | `1000` |
| `anofox_email_dns_tries` | DNS retries before giving up. | `1` |
| `anofox_email_smtp_port` | SMTP port used for MX hosts. | `25` |
| `anofox_email_smtp_connect_timeout_ms` | TCP connect timeout (1–5000 ms). | `5000` |
| `anofox_email_smtp_read_timeout_ms` | Read/write timeout (1–5000 ms). | `5000` |
| `anofox_email_smtp_helo_domain` | HELO/EHLO domain presented to servers. | `duckdb.local` |
| `anofox_email_smtp_mail_from` | MAIL FROM address used during verification. | `validator@duckdb.local` |
| `anofox_postal_data_path` | Directory where libpostal assets are stored. | `.duckdb/extensions/libpostal` |
| `anofox_phonenumber_default_region` | ISO region used when the hint is NULL. | `US` |

### Libpostal Assets

Libpostal requires data files (~500 MB). Either download them manually to `anofox_postal_data_path` or let the extension do it:

```sql
CALL anofox_postal_load_data(); -- returns true on success
```

Ensure the DuckDB process has write permissions in the target directory.

## Data Diffing

Anofox Tabular provides SQL-based data diffing capabilities for comparing tables and identifying differences. This feature is inspired by [Datafold's data-diff](https://www.datafold.com/data-diff) tool.

### Quick Example

```sql
-- Compare two tables and show differences
WITH diff_result AS (
    SELECT
        CASE
            WHEN s.id IS NULL THEN 'added'
            WHEN t.id IS NULL THEN 'removed'
            WHEN hash(s.id, s.name, s.value) != hash(t.id, t.name, t.value) THEN 'changed'
            ELSE 'unchanged'
        END AS diff_type,
        COALESCE(s.id, t.id) AS id,
        COALESCE(t.name, s.name) AS name,
        COALESCE(t.value, s.value) AS value
    FROM source_table s
    FULL OUTER JOIN target_table t
    ON s.id = t.id
)
SELECT * FROM diff_result WHERE diff_type != 'unchanged';
```

### Features

- **Simple SQL Queries**: Uses DuckDB's FULL OUTER JOIN for efficient comparison
- **Multiple Primary Keys**: Supports both single and compound primary keys
- **Column Selection**: Compare all columns or just specific ones for better performance
- **Summary Statistics**: Get counts of added, removed, changed, and unchanged rows
- **Best Practices**: Documented patterns for migration validation, regression testing, and change detection

### Documentation

For comprehensive examples and best practices, see [`docs/DATA_DIFF_GUIDE.md`](docs/DATA_DIFF_GUIDE.md), which includes:
- Single and compound primary key comparisons
- Column-specific diffing for performance
- Summary statistics queries
- Data migration validation examples
- Regression testing patterns
- Incremental change detection
- Performance optimization tips

### Roadmap

**Phase 1 (Complete)**: SQL-based implementation with comprehensive examples
**Phase 2 (Planned)**: C++ table-in-out functions with bisection algorithm for cross-database comparison

## Development & Testing

- `make` – configure and build release binaries with the extension.
- `make test` – run the SQL test suite under `test/sql`.
- `cmake --build build/release --config Debug` – build a debug flavour if needed.

All tracing is disabled via `SET anofox_trace_enabled = false` during tests if noise becomes an issue.

## Distribution Checklist

1. Build the loadable bundle: `cmake --build build/release --target anofox_tabular_loadable_extension`.
2. Upload `anofox_tabular.duckdb_extension` alongside the versioned metadata generated by DuckDB’s tooling.
3. Users install via DuckDB’s standard `INSTALL/LOAD` commands once the custom repository path is set.

---

This repository started from the DuckDB extension template but now serves as a reference implementation for data-quality focused tooling inside DuckDB. Contributions and bug reports are welcome.
