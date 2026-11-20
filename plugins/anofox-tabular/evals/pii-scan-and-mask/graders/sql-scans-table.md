---
# answer.sql scans for PII; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'pii_(?:scan|audit)_table\s*\(|pii_(?:detect|count|contains)\w*\s*\('
flags: i
---
