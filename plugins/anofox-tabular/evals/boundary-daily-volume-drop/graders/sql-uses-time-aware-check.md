---
# answer.sql uses metric_anomaly_iqr or rel_count_change; scripts/run_plugin_evals.py also executes it against evals/fixtures/fixture.sql
type: regex
target: { source: file, path: answer.sql }
pattern: 'metric_anomaly_iqr\w*\s*\(|rel_count_change\w*\s*\('
flags: i
---
