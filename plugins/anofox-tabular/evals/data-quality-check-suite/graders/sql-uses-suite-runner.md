---
# answer.sql runs the suite with run_checks; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'run_checks\s*\('
flags: i
---
