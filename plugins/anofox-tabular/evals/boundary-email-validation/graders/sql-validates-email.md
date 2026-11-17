---
# answer.sql validates emails; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'email_(?:is_valid|validate)\s*\('
flags: i
---
