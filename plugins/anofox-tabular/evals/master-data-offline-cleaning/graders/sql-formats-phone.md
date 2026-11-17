---
# answer.sql formats phone numbers; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'phonenumber_(?:format|parse)\s*\('
flags: i
---
