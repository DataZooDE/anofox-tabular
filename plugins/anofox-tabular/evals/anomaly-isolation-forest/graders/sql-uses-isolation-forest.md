---
# answer.sql calls isolation_forest; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'isolation_forest(?:_mv)?\s*\('
flags: i
---
