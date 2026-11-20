---
# answer.sql calls diff_schema or schema_check; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'diff_schema\s*\(|schema_check\s*\('
flags: i
---
