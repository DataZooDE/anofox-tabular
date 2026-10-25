# Postal Addresses

> Parse and normalize free-text postal addresses with libpostal.

**Use this page to:**
- Download the libpostal data the module needs
- Split an address string into house number, road, city, state, postcode and country
- Generate normalized variants of an address for matching

> **Data download first.** Address parsing needs the libpostal data files, about 500 MB.
> Download them once with `SELECT postal_load_data();` before you call `postal_parse_address`
> or `postal_expand_address`. If the data is missing, the first parse or expand call starts the
> same download itself. See [Network and privacy](../reference/network-and-privacy.md).

The module wraps [libpostal](https://github.com/openvenues/libpostal). It is compiled in only
when libpostal is found at build time. It is not available on Windows. On macOS the CI script
tries to build libpostal from source, so check your build: when the module is missing, the four
postal functions and the setting `anofox_tab_postal_data_path` are not registered.

```sql
SELECT count(*) > 0 AS postal_available
FROM duckdb_functions() WHERE function_name = 'anofox_tab_postal_parse_address';
```

```text
┌──────────────────┐
│ postal_available │
│     boolean      │
├──────────────────┤
│ true             │
└──────────────────┘
```

> **How this page was checked.** The examples that need the data are marked `no-run`. The
> `postal_parse_address` results in the table below come from the test file
> `test/sql/anofox_postal_parse_labels.test`, which runs when the environment variable
> `POSTAL_DATA_PATH` points to a downloaded data directory; the `221B Baker Street` example is
> taken from `test/sql/anofox_postal.test.disabled`, which the test suite does not run today.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`postal_load_data`](#postal_load_data) | `anofox_tab_postal_load_data` | scalar | `BOOLEAN` |
| [`postal_status`](#postal_status) | `anofox_tab_postal_status` | table | initialization and data status |
| [`postal_parse_address`](#postal_parse_address) | `anofox_tab_postal_parse_address` | scalar | `STRUCT` of address parts |
| [`postal_expand_address`](#postal_expand_address) | `anofox_tab_postal_expand_address` | scalar | `VARCHAR[]` |

## Where the data goes

The setting `anofox_tab_postal_data_path` sets the data directory. Its default,
`.duckdb/extensions/libpostal`, is relative to the working directory of the DuckDB process.
The directory and any missing parent directories are created. A path that starts
with `~` is expanded to your home directory. Set the path before the first postal call:

```sql no-run
-- Keep the data in one place for every working directory
SET anofox_tab_postal_data_path = '~/.duckdb/extensions/libpostal';
SELECT postal_load_data();
```

The files come from `https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/`:
`language_classifier.tar.gz`, `libpostal_data.tar.gz` and `parser.tar.gz`.

---

## `postal_load_data`

Canonical name: `anofox_tab_postal_load_data`. Scalar function.

Downloads and unpacks the libpostal data into `anofox_tab_postal_data_path`. Returns `true`
when the data is present afterwards. If the data is already there, it does not download again.

```text
postal_load_data() -> BOOLEAN
```

```sql no-run
-- Downloads about 500 MB on the first call
SELECT postal_load_data();
```

---

## `postal_status`

Canonical name: `anofox_tab_postal_status`. Table function.

```text
postal_status() -> TABLE(initialized BOOLEAN, data_present BOOLEAN, data_dir VARCHAR)
```

| Column | Type | Description |
|--------|------|-------------|
| `initialized` | `BOOLEAN` | libpostal has been set up in this process |
| `data_present` | `BOOLEAN` | the data files exist in `data_dir` |
| `data_dir` | `VARCHAR` | the data directory in use |

On a fresh installation:

```sql
SELECT * FROM postal_status();
```

```text
┌─────────────┬──────────────┬──────────────────────────────┐
│ initialized │ data_present │           data_dir           │
│   boolean   │   boolean    │           varchar            │
├─────────────┼──────────────┼──────────────────────────────┤
│ false       │ false        │ .duckdb/extensions/libpostal │
└─────────────┴──────────────┴──────────────────────────────┘
```

---

## `postal_parse_address`

Canonical name: `anofox_tab_postal_parse_address`. Scalar function.

Splits an address into its parts. libpostal lower-cases the parts. A part that libpostal does
not find is `NULL`.

```text
postal_parse_address(address VARCHAR) -> STRUCT(
    house_number VARCHAR,
    road         VARCHAR,
    city         VARCHAR,
    state        VARCHAR,
    postcode     VARCHAR,
    country      VARCHAR
)
```

```sql no-run
-- Needs the libpostal data (see above)
SELECT (p).house_number, (p).road, (p).city, (p).postcode
FROM (SELECT postal_parse_address('221B Baker Street, London') AS p);
-- house_number = '221b', road = 'baker street', city = 'london', postcode = NULL
```

libpostal tags every token of the address and can give the same label to more than one part.
The values of a repeated label are joined with a space, in the order libpostal returns them; no
part is dropped. libpostal labels are not always right, and the join keeps its mistakes visible
instead of hiding them. Results with the libpostal 1.1 data, from the test file
`test/sql/anofox_postal_parse_labels.test`:

| Address | `house_number` | `road` | `city` | `postcode` | `country` |
|---------|----------------|--------|--------|------------|-----------|
| `Prinsengracht 263, 1016 GV Amsterdam` | `263` | `prinsengracht gv` | `amsterdam` | `1016` | `NULL` |
| `Hauptstraße 5, 80331 München, Deutschland` | `5` | `hauptstraße` | `münchen` | `80331` | `deutschland` |
| `10 Downing Street, London SW1A 2AA, United Kingdom` | `10` | `downing street` | `london` | `sw1a 2aa` | `united kingdom` |
| `Flat 3, 12 High Street, Oxford OX1 4AA` | `12 4aa` | `high street ox1` | `oxford` | `NULL` | `NULL` |

libpostal splits the Dutch postcode `1016 GV` into `postcode` `1016` and `road` `gv`, and does
not recognise the UK postcode `OX1 4AA` after `Oxford`. Writing the Dutch postcode without the
space (`1016GV`) gives `postcode` `1016gv`. Labels other than the six columns (for example
`unit` or `level`) are not returned.

A `NULL` address returns `NULL` without loading the data:

```sql
SELECT postal_parse_address(NULL) IS NULL AS is_null;
```

```text
┌─────────┐
│ is_null │
│ boolean │
├─────────┤
│ true    │
└─────────┘
```

---

## `postal_expand_address`

Canonical name: `anofox_tab_postal_expand_address`. Scalar function.

Returns normalized variants of an address, for example with abbreviations expanded. Two
addresses that share a variant probably refer to the same place.

```text
postal_expand_address(address VARCHAR) -> VARCHAR[]
```

```sql no-run
-- Needs the libpostal data (see above)
SELECT postal_expand_address('221B Baker St, London');
```

## See also

- [Configuration: postal settings](../reference/configuration.md#postal)
- [Troubleshooting: libpostal data missing](../reference/troubleshooting.md#libpostal-data-missing)
- [Conventions](00-conventions.md)
