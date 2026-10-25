# Troubleshooting

> Common errors and how to fix them.

**Use this page to:**
- Get the extension installed and loaded
- Fix errors about missing tables, data or models
- Quiet the log output

## The extension is not found

```text
Catalog Error: ... anofox_tabular ... not found
```

Install it from the DuckDB community repository, then load it:

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

To upgrade to the newest release, install again with `FORCE`:

```sql no-run
FORCE INSTALL anofox_tabular FROM community;
```

The extension is built for DuckDB v1.5.6 and DuckDB 2.0, on `linux_amd64`, `osx_arm64`,
`osx_amd64` and `windows_amd64`. Other DuckDB versions or platforms cannot load it. Check your
version with `SELECT version();`.

## `[anofox] [info]` lines in the output

Tracing is on at level `info` by default and writes to stderr. To show only warnings and errors:

```sql
SET anofox_tab_trace_level = 'warn';
```

Or turn it off with `SET anofox_tab_trace_enabled = false;`. See
[Configuration: tracing](configuration.md#tracing).

## "Table with name ... does not exist" for a TEMP table

Several table functions read the data through a second connection, which cannot see the TEMP
tables of your session:

```sql
CREATE TEMP TABLE scratch AS SELECT range * 1.0 AS v FROM range(50);
SELECT status FROM isolation_forest('scratch', 'v');
```

```text
Binder Error: isolation_forest: cannot read table 'scratch': Catalog Error: Table with name scratch does not exist!
Did you mean "sqlite_schema"?

LINE 1: SELECT * FROM query_table('scratch') LIMIT 0
             ^^^^^^^

LINE 1: SELECT status FROM isolation_forest('scratch', 'v');
                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Use a regular table (`CREATE TABLE`) instead. The functions that need one are listed in
[API conventions](../api/00-conventions.md#table-and-column-arguments).

## "Invalid named parameter"

Most parameters are positional. Only a few accept `name := value`; the list is in
[API conventions](../api/00-conventions.md#named-parameters).

## libpostal data missing

`postal_parse_address` and `postal_expand_address` need about 500 MB of data. Download it once:

```sql no-run
SELECT postal_load_data();
```

The data goes to `anofox_tab_postal_data_path`, by default relative to the working directory of
the process. To keep one copy for every working directory, set an absolute path before the first
postal call:

```sql no-run
SET anofox_tab_postal_data_path = '~/.duckdb/extensions/libpostal';
SELECT postal_load_data();
```

The postal functions are not available on Windows, and may be missing on macOS builds. See
[Postal addresses](../api/02-postal.md).

## Names, organizations or locations are not detected

These PII types need the NER model. Check its state:

```sql
SELECT onnx_available, model_status FROM anofox_ner_status();
```

```text
┌────────────────┬──────────────┐
│ onnx_available │ model_status │
│    boolean     │   varchar    │
├────────────────┼──────────────┤
│ true           │ NOT_LOADED   │
└────────────────┴──────────────┘
```

| `model_status` | Meaning |
|----------------|---------|
| `NOT_LOADED` | The model loads on the first NER-based detection |
| `DOWNLOADING` | The model is being downloaded |
| `LOADED` | NER is active |
| `NOT_DOWNLOADED` | The model is not on disk and `anofox_ner_auto_download` is `false` |
| `FAILED` | Download or load failed; `status_message` says why |
| `NOT_AVAILABLE` | This build has no OpenVINO (`onnx_available = false`) |

The model handles English text and reads the first 512 tokens of a value. See
[PII detection](../api/06-pii.md#anofox_ner_status).

## SMTP checks time out

The `smtp` mode connects to the mail host, usually on port 25, which many networks block. The
SMTP timeouts default to 5000 ms, which is also their maximum; larger values are rejected:

```sql
SET anofox_tab_email_smtp_connect_timeout_ms = 10000;
```

```text
Invalid Input Error: anofox_tab_email_smtp_connect_timeout_ms must be between 1 and 5000
```

If port 25 is blocked, use the `dns` mode, which checks the MX records only.

## Error messages that end with "Unexpected? Please report it"

Some errors end with a link to the issue tracker, also for wrong arguments. If the error is
about your input, fix the call. If it looks like a bug, please
[open an issue](https://github.com/DataZooDE/anofox-tabular/issues) with the query and a small
sample of the data.

## See also

- [Configuration](configuration.md)
- [Network and privacy](network-and-privacy.md)
- [Getting started](../guides/01-getting-started.md)
