# Testing

> Run the SQL tests, the NER tests and the Python tests.

**Use this page to:**
- Run the whole SQL suite or one file
- Run the tests that need the NER model or libpostal
- Run the Python package tests

## SQL tests

The tests are [sqllogictest](https://duckdb.org/docs/stable/dev/sqllogictest/intro) files in
[`test/sql/`](../../test/sql). Each starts with `require anofox_tabular`.

```bash
make test           # release build, all of test/
make test_debug     # debug build
```

`make test` sets `DATAZOO_DISABLE_TELEMETRY=1`, so test runs send no telemetry. To run one file:

```bash
DATAZOO_DISABLE_TELEMETRY=1 ./build/release/test/unittest test/sql/anofox_vat.test
```

The test runner changes its working directory to the checkout it was built from. When you run a
`unittest` built in another checkout (for example a git worktree), pass the directory:

```bash
DATAZOO_DISABLE_TELEMETRY=1 /path/to/build/release/test/unittest --test-dir "$PWD" test/sql/docs_coverage.test
```

## Tests that need extra data

| Tests | Guard | How to run |
|-------|-------|------------|
| NER: every test with `require-env OPENVINO_AVAILABLE` (`anofox_pii_ner*.test`, `anofox_ner_*.test`, ...) | `require-env OPENVINO_AVAILABLE` | `make test_ner`. Needs a build with OpenVINO and downloads the default NER model `bert-multi` (~178 MB) on first run. |
| Non-default NER models: `anofox_ner_model_distilbert_en.test`, `anofox_ner_model_distilbert_multi.test` | `require-env OPENVINO_AVAILABLE` and `require-env ANOFOX_TEST_NER_MODELS` | `make test_ner_models`. Downloads 66 MB and 135 MB on first run. The NER model is chosen once per process, so each file runs in a process of its own; do not set `ANOFOX_TEST_NER_MODELS` for a whole-suite run. |
| NER offline: `anofox_pii_ner_offline.test` | `require-env ANOFOX_TEST_NER_OFFLINE` | Needs a home directory without the model: `HOME=$(mktemp -d) ANOFOX_TEST_NER_OFFLINE=1 build/release/test/unittest test/sql/anofox_pii_ner_offline.test` |
| Postal: `anofox_postal_null.test` | `require-env POSTAL_AVAILABLE` | `make test` sets `POSTAL_AVAILABLE=1` when libpostal is installed |
| Postal data path: `anofox_postal_data_path.test` | `require-env POSTAL_AVAILABLE` and `require-env ANOFOX_TEST_POSTAL_FRESH_PROCESS` | libpostal is initialised once per process and its data path cannot change afterwards, so `make test` runs this file in a process of its own with `ANOFOX_TEST_POSTAL_FRESH_PROCESS=1`; it is skipped in a whole-suite run |

`test/sql/anofox_postal.test.disabled` and `anofox_postal.test_disabled` are not run, because
they need the ~500 MB libpostal data.

## NER model benchmark

`test/ner_eval/` holds a manual benchmark of the NER models (precision and recall for person,
organization and location names on 42 hand-annotated German, English, French, Spanish, Italian
and Dutch texts, and the time per row). It is not run by any test target; see
[`test/ner_eval/README.md`](../../test/ner_eval/README.md) for how to run it and the last
results.

## Docs coverage test

`test/sql/docs_coverage.test` fails when a registered function or setting is missing from the
docs:

- every function named `anofox%` and every short alias must appear as `` `name` `` or
  `` `name(`` in `docs/api/*.md`;
- every setting named `anofox%` and `datazoo_banner` must appear as `` `name` `` in
  `docs/reference/configuration.md`.

The test prints the missing names. When you add a function or a setting, document it on the
module page and in the configuration reference, and the test passes again.

## Docs snippet checker

`make check-docs` runs `scripts/check_doc_snippets.py` (after `GEN=ninja make release`). It
executes every ```` ```sql ```` fence of `README.md`, `docs/**/*.md` (except `docs/reviews/`) and
the Claude Code skills in `plugins/anofox-tabular/skills/`, each in a fresh in-memory database of
`build/release/duckdb` with the extension loaded from its file, and checks the function names
they mention:

- A block runs after the earlier blocks of the same page that create the tables it uses, so write
  each page's examples in order and create the sample data in the page.
- A block followed by a ```` ```text ```` block whose first line is an error
  (`Binder Error: ...`) must raise that error.
- `` `anofox_*` `` code spans and calls such as `email_validate(` must name a registered function
  or setting. Names that are none (the extension itself, source files) are listed in
  `scripts/doc_snippets/allowed_names.txt`.
- The check runs offline: the NER model is not downloaded
  (`anofox_ner_auto_download = false`), and HOME is a temporary directory. These blocks are
  skipped unless you set the variable: postal calls (`POSTAL_AVAILABLE=1`, downloads the
  libpostal data), email validation in the `dns` or `smtp` mode (`ANOFOX_DOCS_NETWORK=1`), and
  blocks keyed in `scripts/doc_snippets/requires.txt` (`postal`, `ner` with
  `ANOFOX_NER_AVAILABLE=1`, `network`).
- ```` ```sql no-run ```` fences (install commands, paths on your machine) are not run.
  Signatures belong in ```` ```text ```` fences.

`scripts/doc_snippets/` also holds `fixtures.sql` (run before every block), `synopsis.txt` and
`expected_errors.txt` (blocks keyed by `file<TAB>first SQL line` that are not run, or must fail
without showing the error). The script writes every block's status to
`build/doc-snippets.tsv`; pass file names to check only those files, or
`--changed-since origin/main`. The DuckDB 2.0 workflow (`Next20Tests.yml`) runs it after the SQL
tests.

`make check-plugin` (`scripts/check_plugin.py`) needs no build and runs in the Static audits
workflow: it checks that the plugin manifests parse, that each skill's frontmatter has its
directory name, a description with a "Use when" sentence and the plugin version, that the plugin
version matches `python/pyproject.toml`, and that README.md and CLAUDE.md list every skill.

## Python tests

Use uv. Build the extension first, then:

```bash
cd python
uv sync --extra dev
ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  uv run pytest tests/ -v

# tests that do not need the extension
uv run pytest tests/test_loader.py tests/test_utils.py -v
```

## Next steps

- [Building](building.md)
- [Architecture](architecture.md)
