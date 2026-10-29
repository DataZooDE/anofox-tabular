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
