---
# answer.sql looks at amounts per customer_id; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'customer_id'
flags: i
---
