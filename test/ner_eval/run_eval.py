#!/usr/bin/env python3
"""Manual NER model benchmark: precision, recall and speed per NER model.

Runs every text of texts.json through pii_detect (types NAME, ORGANIZATION and
LOCATION only) once per model, each model in its own DuckDB process (the model
cannot be switched after it has been loaded), and compares the matches with
the hand-annotated PER / ORG / LOC spans.

Not part of CI: the models are downloaded from Hugging Face on first use
(66-178 MB each, cached in ~/.duckdb/extensions/anofox/ner/<model>/), and the
timings depend on the machine. See README.md in this directory.

Usage (from the repository root, after `GEN=ninja make release`):
    uv run --no-project python test/ner_eval/run_eval.py
    uv run --no-project python test/ner_eval/run_eval.py --models distilbert-en,bert-multi --repeat 5
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_MODELS = ["distilbert-en", "distilbert-multi", "bert-multi"]
GOLD_TYPES = {"PER": "NAME", "ORG": "ORGANIZATION", "LOC": "LOCATION"}
# Dictionary name matches (the fallback when the model finds no person) have
# confidence 0.5; NER matches are never reported below 0.7
NER_MIN_CONFIDENCE = 0.7


def gold_spans(text: str, surfaces: list[str]) -> list[tuple[int, int]]:
    """All occurrences of the annotated surfaces as UTF-8 byte spans, without
    spans contained in a longer annotated span ("Weiß" inside "Sabine Weiß")."""
    data = text.encode("utf-8")
    spans = set()
    for surface in surfaces:
        needle = surface.encode("utf-8")
        start = data.find(needle)
        if start < 0:
            raise ValueError(f"annotation {surface!r} not found in {text[:60]!r}")
        while start >= 0:
            spans.add((start, start + len(needle)))
            start = data.find(needle, start + 1)
    return sorted(s for s in spans if not any(o != s and o[0] <= s[0] and s[1] <= o[1] for o in spans))


def overlaps(a: tuple[int, int], b: tuple[int, int]) -> bool:
    return a[0] < b[1] and b[0] < a[1]


def score(gold: list[tuple[int, int]], predicted: list[tuple[int, int]]) -> dict[str, int]:
    """Exact and overlap counts for one text and one type.

    Overlap: a gold span is found when a prediction overlaps it; a prediction
    is correct when it overlaps a gold span. Exact: same byte span."""
    return {
        "gold": len(gold),
        "pred": len(predicted),
        "tp_gold": sum(any(overlaps(g, p) for p in predicted) for g in gold),
        "tp_pred": sum(any(overlaps(g, p) for g in gold) for p in predicted),
        "exact": len(set(gold) & set(predicted)),
    }


def run_model(cli: list[str], model: str, texts_path: Path, repeat: int, out_dir: Path) -> tuple[dict, list[float]]:
    out_file = out_dir / f"{model}.json"
    # The NER result cache stays on (as in normal use: NAME, ORGANIZATION and
    # LOCATION share one model run per text) but is emptied before each run
    timed = "\n".join(
        "SET anofox_ner_cache_size = 0;\nSET anofox_ner_cache_size = 10000;\n.timer on\n"
        f"CREATE OR REPLACE TABLE run{i} AS SELECT id, pii_detect(text) AS matches FROM texts;\n.timer off"
        for i in range(repeat)
    )
    sql = f"""
SET anofox_tab_trace_level = 'warn';
SET threads = 1;
SET anofox_ner_model = '{model}';
SET anofox_pii_enabled_types = 'NAME,ORGANIZATION,LOCATION';
CREATE TABLE texts AS SELECT id, text FROM read_json('{texts_path}');
-- load the model (and download it on first use) outside the timed runs
SELECT len(pii_detect('Warm-up: Sabine Weiß wohnt in München.')) AS warm_up;
SELECT model_status, status_message FROM anofox_ner_status();
{timed}
COPY (SELECT id, matches FROM run0) TO '{out_file}' (FORMAT json);
"""
    proc = subprocess.run(cli + ["-batch"], input=sql, capture_output=True, text=True, cwd=ROOT)
    if proc.returncode != 0 or not out_file.exists():
        sys.exit(f"{model}: DuckDB failed\n{proc.stdout}\n{proc.stderr}")
    if "LOADED" not in proc.stdout:
        sys.exit(f"{model}: NER model not loaded\n{proc.stdout}\n{proc.stderr}")
    times = [float(t) for t in re.findall(r"Run Time \(s\): real ([0-9.]+)", proc.stdout + proc.stderr)]
    if len(times) != repeat:
        sys.exit(f"{model}: expected {repeat} timings, got {times}\n{proc.stdout}")
    results = {}
    for line in out_file.read_text().splitlines():
        row = json.loads(line)
        results[row["id"]] = row["matches"] or []
    return results, times


def evaluate(texts: list[dict], results: dict, include_dictionary: bool) -> dict[str, dict[str, int]]:
    totals = {t: {"gold": 0, "pred": 0, "tp_gold": 0, "tp_pred": 0, "exact": 0} for t in GOLD_TYPES}
    for item in texts:
        matches = results.get(item["id"], [])
        for gold_type, pii_type in GOLD_TYPES.items():
            gold = gold_spans(item["text"], item[gold_type])
            predicted = sorted(
                {
                    (m["start_pos"], m["end_pos"])
                    for m in matches
                    if m["type"] == pii_type and (include_dictionary or m["confidence"] >= NER_MIN_CONFIDENCE)
                }
            )
            for key, value in score(gold, predicted).items():
                totals[gold_type][key] += value
    return totals


def ratio(numerator: int, denominator: int) -> str:
    return f"{numerator / denominator:.2f}" if denominator else "-"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--models", default=",".join(DEFAULT_MODELS))
    parser.add_argument("--repeat", type=int, default=3, help="timed runs over all texts per model")
    parser.add_argument("--lang", default="", help="only texts of these languages, e.g. de or de,en")
    parser.add_argument("--details", action="store_true", help="print the matches per text")
    args = parser.parse_args()

    cli = os.environ.get("DUCKDB_CLI", str(ROOT / "scripts" / "duckdb-dev.sh")).split()
    texts_path = HERE / "texts.json"
    texts = json.loads(texts_path.read_text())
    if args.lang:
        languages = set(args.lang.split(","))
        texts = [t for t in texts if t["lang"] in languages]
    for item in texts:  # validate the annotations before running anything
        for gold_type in GOLD_TYPES:
            gold_spans(item["text"], item[gold_type])

    os.environ.setdefault("DATAZOO_DISABLE_TELEMETRY", "1")

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        subset = Path(tmp) / "texts.json"
        subset.write_text(json.dumps(texts, ensure_ascii=False))
        for model in args.models.split(","):
            results, times = run_model(cli, model, subset, args.repeat, Path(tmp))
            ner = evaluate(texts, results, include_dictionary=False)
            with_dict = evaluate(texts, results, include_dictionary=True)
            ms_per_row = statistics.median(times) * 1000 / len(texts)
            rows.append((model, ner, with_dict, ms_per_row))
            if args.details:
                for item in texts:
                    found = [f"{m['type'][:3]}:{m['text']}" for m in results.get(item["id"], [])]
                    print(f"{model} #{item['id']} {found}")

    n_chars = sum(len(t["text"]) for t in texts)
    print(f"{len(texts)} texts ({n_chars} characters), languages: {sorted({t['lang'] for t in texts})}")
    gold = rows[0][1]
    print("gold spans: " + ", ".join(f"{t} {gold[t]['gold']}" for t in GOLD_TYPES))
    print()
    print("| model | PER P | PER R | PER R exact | PER R incl. dictionary | ORG P | ORG R | LOC P | LOC R | ms/row |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for model, ner, with_dict, ms in rows:
        per, org, loc = ner["PER"], ner["ORG"], ner["LOC"]
        print(
            f"| {model} | {ratio(per['tp_pred'], per['pred'])} | {ratio(per['tp_gold'], per['gold'])} "
            f"| {ratio(per['exact'], per['gold'])} | {ratio(with_dict['PER']['tp_gold'], with_dict['PER']['gold'])} "
            f"| {ratio(org['tp_pred'], org['pred'])} | {ratio(org['tp_gold'], org['gold'])} "
            f"| {ratio(loc['tp_pred'], loc['pred'])} | {ratio(loc['tp_gold'], loc['gold'])} | {ms:.1f} |"
        )


if __name__ == "__main__":
    main()
