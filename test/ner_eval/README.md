# NER model benchmark

A manual benchmark of the NER models that `anofox_ner_model` can select (default `bert-multi`): precision and recall
for persons (PER), organizations (ORG) and locations (LOC), and the time per row. It is not
part of CI: the models are downloaded from Hugging Face on first use, and the timings depend
on the machine.

- `texts.json`: 42 short and long texts with hand-annotated PER, ORG and LOC spans. 24 German
  insurance and support tickets and letters (salutations, signatures, names with umlauts and ß,
  double names, GmbH/AG companies, cities and streets, a long complaint letter, a ticket
  history), 10 English tickets and emails, and 8 French, Spanish, Italian and Dutch lines.
  Four texts contain no entity at all.
- `run_eval.py`: runs every text through `pii_detect` with only `NAME`, `ORGANIZATION` and
  `LOCATION` enabled, once per model in its own DuckDB process, and compares the matches with
  the annotations.

## Run it

From the repository root, after `GEN=ninja make release` with OpenVINO:

```bash no-run
uv run --no-project python test/ner_eval/run_eval.py
uv run --no-project python test/ner_eval/run_eval.py --models bert-multi --lang de --details
```

`--repeat` sets the number of timed runs (median), `--lang` restricts the texts to languages,
`--details` prints the matches per text. `DUCKDB_CLI` overrides the shell
(default `scripts/duckdb-dev.sh`).

## Scoring

- Recall: an annotated span is found when a match of the same type overlaps it. "PER R exact"
  counts only matches with exactly the annotated byte span.
- Precision: a match is correct when it overlaps an annotated span of the same type.
- NER matches only (confidence 0.7 and higher), except "PER R incl. dictionary", which also
  counts the name-dictionary fallback (confidence 0.5) that runs when the model finds no person.
- ms/row: median of the timed runs over all texts with one DuckDB thread, the NER result cache
  emptied before each run, divided by the number of texts. A text runs through the model once
  (the three types share the cached result); texts over 256 tokens take one run per window.

## Results

AMD Ryzen 9 3950X (16 cores, AVX2), OpenVINO 2025.3 (vcpkg, static), CPU device,
2026-10-27.

All 42 texts (PER 55, ORG 32, LOC 40 annotated spans):

| Model | Size | Languages | PER P | PER R | PER R exact | ORG P | ORG R | LOC P | LOC R | ms/row |
|---|---|---|---|---|---|---|---|---|---|---|
| `distilbert-en` | 66 MB | en | 0.55 | 0.65 | 0.44 | 0.33 | 0.59 | 1.00 | 0.50 | 20.7 |
| `distilbert-multi` | 135 MB | 10 | 0.93 | 0.93 | 0.91 | 0.90 | 0.81 | 1.00 | 0.95 | 48.5 |
| `bert-multi` | 178 MB | 10 | 0.94 | 0.93 | 0.91 | 0.90 | 0.84 | 1.00 | 0.93 | 40.3 |

German texts (24 texts; PER 31, ORG 15, LOC 22):

| Model | PER P | PER R | PER R exact | PER R incl. dictionary | ORG P | ORG R | LOC P | LOC R | ms/row |
|---|---|---|---|---|---|---|---|---|---|
| `distilbert-en` | 0.38 | 0.55 | 0.26 | 0.65 | 0.21 | 0.53 | 1.00 | 0.50 | 26.3 |
| `distilbert-multi` | 0.90 | 0.90 | 0.90 | 0.90 | 0.85 | 0.73 | 1.00 | 1.00 | 53.1 |
| `bert-multi` | 1.00 | 0.87 | 0.84 | 0.87 | 0.82 | 0.93 | 1.00 | 0.95 | 40.6 |

English texts (10 texts; PER 12, ORG 9, LOC 10):

| Model | PER P | PER R | ORG P | ORG R | LOC P | LOC R | ms/row |
|---|---|---|---|---|---|---|---|
| `distilbert-en` | 1.00 | 0.92 | 0.90 | 1.00 | 1.00 | 0.80 | 16.2 |
| `distilbert-multi` | 1.00 | 0.92 | 0.90 | 1.00 | 1.00 | 0.90 | 51.8 |
| `bert-multi` | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0.90 | 41.6 |

French, Spanish, Italian and Dutch texts (8 texts; PER 12, ORG 8, LOC 8):

| Model | PER P | PER R | ORG P | ORG R | LOC P | LOC R | ms/row |
|---|---|---|---|---|---|---|---|
| `distilbert-en` | 0.80 | 0.67 | 0.25 | 0.25 | 1.00 | 0.12 | 15.0 |
| `distilbert-multi` | 0.92 | 1.00 | 1.00 | 0.75 | 1.00 | 0.88 | 45.2 |
| `bert-multi` | 0.80 | 1.00 | 1.00 | 0.50 | 1.00 | 0.88 | 25.6 |

Notes:

- On German text `distilbert-en` takes capitalised nouns for names and organizations
  ("Sehr", "Grüßen Jürgen", "München" as an organization) and splits names; only about a quarter
  of the German names are found with their exact span.
- `distilbert-multi` runs at a fixed length of 256 tokens on this OpenVINO build: with a dynamic
  sequence length every inference fails in the first attention MatMul ("bad mem size"), so the
  extension compiles it for a fixed length (see the warning in the trace). With a dynamic length
  (pip OpenVINO 2025.3 on the same machine) it takes about 17 ms per text, `distilbert-en` 18 ms
  and `bert-multi` 31 ms; here the padding to 256 tokens makes it slower than `bert-multi`.
- The multilingual models tag no MISC entities; their DATE tags are not reported.
- Small samples: one missed or extra span moves a number by 0.03 to 0.13. The numbers compare
  the models on this kind of text; they are no general accuracy figures.
