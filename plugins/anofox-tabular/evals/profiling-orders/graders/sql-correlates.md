---
# answer.sql calls profile_correlations; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'profile_correlations\s*\('
flags: i
---
