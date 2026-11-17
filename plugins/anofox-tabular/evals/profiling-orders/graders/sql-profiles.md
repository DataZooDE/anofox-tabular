---
# answer.sql calls profile_table; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'profile_(?:table|summary)\s*\('
flags: i
---
