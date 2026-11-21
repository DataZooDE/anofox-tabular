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
version matches `python/pyproject.toml`, that README.md and CLAUDE.md list every skill, and
that the plugin eval workflow runs on demand only (`workflow_dispatch`).

## Plugin eval suite

`make eval-plugin` (`scripts/run_plugin_evals.py`) tests whether Claude Code loads the right
skill of the plugin for a request and writes SQL that runs. It needs the
[`claude plugin eval`](https://code.claude.com/docs/en/plugin-evals) command (Claude Code
2.1.269 or later), your Claude credentials and a build. Every run is a real model call on your
account: a full run costs about 2 USD (list price) and takes about 3 minutes.

The cases live in `plugins/anofox-tabular/evals/`, one directory per case:

- `prompt.md`: the request (frontmatter: tags, `max_turns`, `allowed_tools`). A request names the
  tables and the task, never a function name, and asks for the SQL in `answer.sql`.
- `graders/skill-invoked.md`: a `tool_used` grader on the `Skill` tool that passes when the
  expected skill was invoked.
- `graders/sql-*.md`: `regex` graders on `answer.sql` for the functions the task needs.

Six cases cover one skill each; the five `boundary-*` cases check requests that sit between two
skills (a daily volume drop is data quality, unusual rows per customer is anomaly detection,
email validation is master data, email masking is PII, a schema drift gate is data quality or
diff). No grader calls a judge model.

`claude plugin eval` has no custom-code graders, so the script adds the SQL check. It runs the
suite with one run per case and no no-plugin baseline (`--ablation none`), keeps the run
workspaces (`--keep-temp`), and then runs `evals/fixtures/fixture.sql` followed by each run's
`answer.sql` with the loader of the docs snippet checker. A run passes when the script exits
without an error and returns at least one row. The kept workspaces are deleted afterwards.

```bash
make eval-plugin                                    # all cases, build/release
make eval-plugin PLUGIN_EVAL_ARGS="--case 'boundary-*' --runs 3"
python3 scripts/run_plugin_evals.py --check-sql my.sql   # one SQL file against the fixture
```

`ANOFOX_DUCKDB` and `ANOFOX_EXTENSION` select another shell and extension file. The results
(`aggregate-result.json`, `report.html` and `sql-results.tsv`) go to
`plugins/anofox-tabular/evals/results/<timestamp>/`, which git ignores. To see why a case failed,
open `report.html`, or pass `--keep` and read the run's transcript. When the `skill-invoked`
grader fails, the skill's `description` does not match the request; fix the description, not
the case.

The Plugin eval workflow (`.github/workflows/plugin-eval.yml`) runs the suite on demand only
(Actions → Plugin eval → Run workflow), with the `ANTHROPIC_API_KEY` repository secret, the
released v2.0.1 linux_amd64 extension and the DuckDB 2.0.1 CLI. Every run costs tokens (about
2 USD), so it never runs on a schedule or on pull requests; `make check-plugin` fails when the
workflow gets another trigger. Without the secret it prints a notice and succeeds.

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
