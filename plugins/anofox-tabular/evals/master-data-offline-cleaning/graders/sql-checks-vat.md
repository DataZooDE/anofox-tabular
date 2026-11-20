---
# answer.sql validates VAT numbers; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'vat_\w+\s*\('
flags: i
---
