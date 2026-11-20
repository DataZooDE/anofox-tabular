---
# answer.sql calls diff_joindiff or diff_hashdiff; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'diff_(?:joindiff|hashdiff)\s*\('
flags: i
---
