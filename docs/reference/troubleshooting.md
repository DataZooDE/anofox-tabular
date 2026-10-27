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

## "TEMP tables are not supported" for a TEMP table

Several table functions read the data through a second connection, which cannot see the TEMP
tables of your session. They reject a TEMP table when the query is bound:

```sql
CREATE TEMP TABLE scratch AS SELECT range * 1.0 AS v FROM range(50);
SELECT status FROM isolation_forest('scratch', 'v');
```

```text
Binder Error: TEMP tables are not supported by isolation_forest; use a regular table (CREATE TABLE ... AS SELECT * FROM scratch)

LINE 1: SELECT status FROM isolation_forest('scratch', 'v');
                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Use a regular table (`CREATE TABLE`) instead. The functions that need one are listed in
[API conventions](../api/00-conventions.md#table-and-column-arguments).

## "reads the last committed data, but this transaction has uncommitted changes"

The same functions see only committed data through their second connection. Inside a
transaction that has inserted, updated or deleted rows, or created a table, they would miss
those changes, so they refuse:

```sql
CREATE TABLE orders AS SELECT i AS order_id, 20.0 + (i % 7) AS amount FROM range(200) t(i);
BEGIN;
INSERT INTO orders VALUES (200, 9999.0);
SELECT status FROM isolation_forest('orders', 'amount');
ROLLBACK;
```

```text
Binder Error: isolation_forest reads the last committed data, but this transaction has uncommitted changes; call it after COMMIT, or outside a transaction

LINE 1: SELECT status FROM isolation_forest('orders', 'amount');
                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

On DuckDB 2.0 any error aborts the open transaction, this one too: `ROLLBACK`, run the load
again and call the function after `COMMIT`. A transaction that has only read data, or has
written only TEMP tables, runs these functions on the committed data.

## Stopping a long call

Ctrl-C in the DuckDB CLI and `Connection.interrupt()` in Python stop the long-running table
functions within a moment: the anomaly functions while they fit or score, `pii_scan_table` and
`pii_audit_table` between input chunks and before each NER inference, the profiling functions
during their queries, and the NER-based scalar functions (`pii_mask`, `pii_detect`, ...) before
each NER inference. The call fails with `INTERRUPT Error: Interrupted!`.

## "Invalid named parameter"

Most parameters are positional. Only a few accept `name := value`; the list is in
[API conventions](../api/00-conventions.md#named-parameters).

## libpostal data missing

`postal_parse_address` and `postal_expand_address` need about 500 MB of data. Download it once:

```sql no-run
SELECT postal_load_data();
```

The data goes to `anofox_tab_postal_data_path`, by default `~/.duckdb/extensions/libpostal` in
your home directory. `postal_status()` shows the directory in use and whether the data is there:

```sql no-run
SELECT data_present, data_dir FROM postal_status();
```

Release v2026.10.25 and older stored the data relative to the working directory, in
`.duckdb/extensions/libpostal`, so every new working directory downloaded it again. A complete
copy there is still used while the home directory has none; move it to
`~/.duckdb/extensions/libpostal` to use it from every working directory. See
[Postal addresses: where the data goes](../api/02-postal.md#where-the-data-goes).

The postal functions are not available on Windows, and may be missing on macOS builds. See
[Postal addresses](../api/02-postal.md).

## "Problem with the SSL CA cert" when downloading the NER model or postal data

```text
[anofox] [warn] ner: Download attempt 1 failed: Problem with the SSL CA cert (path? access rights?) (HTTP 0)
```

The download of the NER model or the libpostal data could not load a list of trusted
certificate authorities (CA bundle). `anofox_ner_status()` then reports `FAILED` with
`Failed to download model`, and `postal_load_data()` fails.

The extension looks for a CA bundle before each download, in this order:

1. The file named by the environment variable `CURL_CA_BUNDLE`, else `SSL_CERT_FILE`. The file is
   used even when it does not exist, so a wrong path fails visibly.
2. The bundle compiled into the bundled libcurl, when that file exists.
3. The first existing file of `/etc/ssl/certs/ca-certificates.crt` (Debian, Ubuntu, Arch),
   `/etc/pki/tls/certs/ca-bundle.crt` (RHEL, Fedora), `/etc/ssl/ca-bundle.pem` (openSUSE),
   `/etc/pki/tls/cacert.pem`, `/etc/ssl/cert.pem` (macOS, Alpine),
   `/usr/local/etc/openssl/cert.pem` and `/opt/homebrew/etc/openssl@3/cert.pem` (Homebrew).

`SSL_CERT_DIR`, when set, adds a directory of certificates. If the error persists, your bundle
is in another place: set the variable before you start DuckDB, for example

```bash
SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt duckdb
```

The trace at level `debug` shows which bundle a download used (`ner: TLS CA bundle ...`).

**Release v2026.10.25 and older.** These releases do not look for a bundle: the Linux build
only knows `/etc/pki/tls/certs/ca-bundle.crt`, which exists on RHEL and Fedora but not on Debian,
Ubuntu or Arch, and the environment variables above have no effect on them. Upgrade
(`FORCE INSTALL anofox_tabular FROM community;`), or work around it in one of two ways:

- Link your system bundle to the expected path (needs root):

  ```bash
  sudo mkdir -p /etc/pki/tls/certs
  sudo ln -s /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt
  ```

- Download the files with the system `curl` into the places the extension reads them from:

  ```bash
  # NER model: the default bert-multi (about 178 MB); for another anofox_ner_model use its
  # name as the directory and its repository from the model table in docs/api/06-pii.md
  # (distilbert-en: onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX,
  # distilbert-multi: Xenova/distilbert-base-multilingual-cased-ner-hrl)
  dir=~/.duckdb/extensions/anofox/ner/bert-multi
  base=https://huggingface.co/Xenova/bert-base-multilingual-cased-ner-hrl/resolve/main
  mkdir -p "$dir"
  curl -L -o "$dir/model_quantized.onnx" "$base/onnx/model_quantized.onnx"
  curl -L -o "$dir/tokenizer.json" "$base/tokenizer.json"

  # libpostal data (about 500 MB); use the directory of anofox_tab_postal_data_path
  dir=~/.duckdb/extensions/libpostal
  mkdir -p "$dir"
  for f in language_classifier.tar.gz libpostal_data.tar.gz parser.tar.gz; do
    curl -L "https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/$f" | tar -xz -C "$dir"
  done
  ```

  `~/.duckdb/extensions/libpostal` is the default data path; if you unpack the data elsewhere,
  `SET anofox_tab_postal_data_path` to that directory before the first postal call.

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
| `FAILED` | Download or load failed; `status_message` says why. After fixing the cause, `SET anofox_ner_device` or `SET anofox_ner_model` makes the next detection load again |
| `NOT_AVAILABLE` | This build has no OpenVINO (`onnx_available = false`) |

A failed load does not stop detection: `NAME` falls back to the name dictionary, and
`ORGANIZATION`, `LOCATION` and `MISC` are not detected. A common cause is a device the machine does
not have (`SET anofox_ner_device = 'GPU'` without a GPU, `status_message` then starts with
`OpenVINO error:`); `SET anofox_ner_device = 'CPU'` (or `'AUTO'`) and run the detection again.
To make a missing model an error instead of a silent fallback, `SET anofox_ner_required = true`.

The default model `bert-multi` handles German, English and eight other languages; values longer
than 256 tokens are read in overlapping windows.
Without the model, `pii_scan_table` reports `scanned (NER unavailable)` or
`no_match (NER unavailable)`. See
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
