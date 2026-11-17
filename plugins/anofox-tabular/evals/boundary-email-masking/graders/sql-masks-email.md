---
# answer.sql masks with pii_mask*; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'pii_(?:mask|redact)\w*\s*\('
flags: i
---
