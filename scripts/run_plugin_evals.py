#!/usr/bin/env python3
"""scripts/run_plugin_evals.py

Runs the `claude plugin eval` suite of the Claude Code plugin (plugins/anofox-tabular/evals/)
and then executes the SQL each run wrote against the eval fixture. `make eval-plugin` and the
Plugin eval workflow (.github/workflows/plugin-eval.yml) call it.

Usage:
    python3 scripts/run_plugin_evals.py [--case GLOB] [--tag TAG] [--runs N] [--model MODEL]
                                        [--max-cost-usd USD] [--threshold T] [--keep]
    python3 scripts/run_plugin_evals.py --check-results DIR/aggregate-result.json
    python3 scripts/run_plugin_evals.py --check-sql FILE.sql

Each case (evals/<case>/prompt.md) names only the tables and the task. The agent runs with
the plugin loaded and the tools Skill, Read, Glob, Grep and Write (Write granted with
--allow-tools), and writes its SQL to answer.sql in its empty workspace. The graders in
evals/<case>/graders/ check, without a judge model, that the expected skill was invoked and
that answer.sql calls the expected functions.

`claude plugin eval` has no custom-code graders, so this script adds the execution check:
the suite runs with --keep-temp, and for each run the script reads answer.sql from the kept
workspace (<kept>/sealed/home/cwd/, found through the run's tracePath in
aggregate-result.json), runs evals/fixtures/fixture.sql followed by answer.sql in the DuckDB
shell with the extension loaded (the loader of scripts/check_doc_snippets.py: in-memory
database, -unsigned, telemetry off, fresh HOME, no downloads) and requires exit code 0 and
at least one line of output. The kept directories are deleted afterwards (--keep keeps them).

Defaults: one run per case, one arm (--ablation none: no no-plugin baseline), the default
model, threshold 1.0, cost ceiling 5 USD. A full run of the 11 cases takes about 3 minutes.

The DuckDB shell and the extension default to build/release/ (`make release`); override with
ANOFOX_DUCKDB and ANOFOX_EXTENSION, as the workflow does with the released binaries.

Writes sql-results.tsv (case, run, status, detail) next to aggregate-result.json in
plugins/anofox-tabular/evals/results/<timestamp>/ (git-ignored). Exit codes: 0 when every
case passed both the graders and the SQL check; 1 when a case or SQL check failed; 2 when the
suite ran only partly (cost ceiling, auth) or a prerequisite is missing.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import stat
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
PLUGIN_DIR = os.path.join(REPO_ROOT, "plugins", "anofox-tabular")
EVALS_DIR = os.path.join(PLUGIN_DIR, "evals")
FIXTURE = os.path.join(EVALS_DIR, "fixtures", "fixture.sql")

sys.path.insert(0, SCRIPT_DIR)
import check_doc_snippets as cds  # noqa: E402  (reuses its shell loader)

cds.DUCKDB_BIN = os.environ.get("ANOFOX_DUCKDB", cds.DUCKDB_BIN)
cds.EXTENSION_FILE = os.environ.get("ANOFOX_EXTENSION", cds.EXTENSION_FILE)

# The extension is loaded from its file already; an INSTALL would go to the network.
INSTALL_SELF_RE = re.compile(
    r"^\s*(?:FORCE\s+)?INSTALL\s+'?anofox_tabular'?(?:\s+FROM\s+\S+)?\s*;\s*$", re.IGNORECASE | re.MULTILINE
)
KEPT_DIR_RE = re.compile(r"^claude-eval-[A-Za-z0-9_-]+$")


def check_sql(sql_text):
    """Run the fixture and sql_text; return (status, detail)."""
    with open(FIXTURE, encoding="utf-8") as f:
        fixture = f.read()
    # headers off: an empty result prints nothing, so "no output" means "no rows"
    script = ".headers off\n" + fixture + "\n" + INSTALL_SELF_RE.sub("", sql_text) + "\n"
    rc, out, err = cds.run_block_safe(script, timeout=180)
    if rc != 0:
        return "FAIL", cds.first_error_line(err, out) or f"exit code {rc}"
    lines = [line for line in out.splitlines() if line.strip() and not line.startswith("[anofox]")]
    if not lines:
        return "FAIL", "the script ran but returned no rows"
    return "PASS", f"{len(lines)} result rows"


def make_readable(path):
    """The eval seals the plugin-written parts of a kept directory (mode 000)."""
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            p = os.path.join(root, name)
            if not os.path.islink(p):
                os.chmod(p, os.stat(p).st_mode | stat.S_IRWXU)


def kept_dir_of(run):
    """<kept>/out/trace.jsonl -> <kept>, only for a claude-eval-* temp directory."""
    trace = run.get("tracePath") or ""
    kept = os.path.dirname(os.path.dirname(trace))
    if not trace or not KEPT_DIR_RE.match(os.path.basename(kept)) or not os.path.isdir(kept):
        return None
    return kept


def check_results(result_json, keep):
    """Execute every run's answer.sql; return (rows, failures)."""
    with open(result_json, encoding="utf-8") as f:
        doc = json.load(f)
    rows, failures, kept_dirs = [], 0, []
    for case in doc.get("cases", []):
        name = case.get("name", "?")
        for i, run in enumerate(case.get("arms", {}).get("with", []), start=1):
            kept = kept_dir_of(run)
            if kept is None:
                status, detail = "FAIL", "no kept workspace (run without --keep-temp?)"
            else:
                kept_dirs.append(kept)
                os.chmod(kept, 0o700)
                make_readable(kept)
                answer = os.path.join(kept, "sealed", "home", "cwd", "answer.sql")
                if not os.path.isfile(answer):
                    status, detail = "FAIL", "the run wrote no answer.sql"
                else:
                    with open(answer, encoding="utf-8", errors="replace") as f:
                        status, detail = check_sql(f.read())
            failures += status != "PASS"
            rows.append((name, i, status, detail))
            print(f"  sql {status:4}  {name} run {i}: {detail}", flush=True)
    if not keep:
        for kept in kept_dirs:
            shutil.rmtree(kept, ignore_errors=True)
    out_tsv = os.path.join(os.path.dirname(result_json), "sql-results.tsv")
    with open(out_tsv, "w", encoding="utf-8") as f:
        f.write("case\trun\tstatus\tdetail\n")
        for r in rows:
            f.write("\t".join(str(x).replace("\t", " ") for x in r) + "\n")
    print(f"SQL check: {len(rows) - failures}/{len(rows)} runs passed ({out_tsv})")
    return rows, failures


def require_build():
    missing = [p for p in (cds.DUCKDB_BIN, cds.EXTENSION_FILE) if not os.path.isfile(p)]
    if missing:
        print("run_plugin_evals: missing " + ", ".join(missing)
              + " (run `make release`, or set ANOFOX_DUCKDB and ANOFOX_EXTENSION)", file=sys.stderr)
        sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--case", action="append", default=[], help="case name glob (repeatable)")
    ap.add_argument("--tag", action="append", default=[], help="run only cases with this tag")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--model", default=None)
    ap.add_argument("--max-cost-usd", default="5")
    ap.add_argument("--threshold", default="1.0")
    ap.add_argument("--keep", action="store_true", help="keep the run workspaces")
    ap.add_argument("--check-results", metavar="JSON", help="only run the SQL check on an existing result")
    ap.add_argument("--check-sql", metavar="FILE", help="run one SQL file against the fixture")
    args = ap.parse_args()

    require_build()
    if args.check_sql:
        with open(args.check_sql, encoding="utf-8") as f:
            status, detail = check_sql(f.read())
        print(f"{status}: {detail}")
        return 0 if status == "PASS" else 1
    if args.check_results:
        _, failures = check_results(args.check_results, args.keep)
        return 1 if failures else 0

    if shutil.which("claude") is None:
        print("run_plugin_evals: the `claude` CLI is not on PATH", file=sys.stderr)
        return 2
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H-%M-%SZ")
    out_dir = os.path.join(EVALS_DIR, "results", stamp)
    cmd = ["claude", "plugin", "eval", PLUGIN_DIR]
    for c in args.case:
        cmd += ["--case", c]
    cmd += [
        "--runs", str(args.runs), "--ablation", "none", "--threshold", args.threshold,
        "--max-cost-usd", args.max_cost_usd, "--trust-plugin", "--keep-temp", "--no-publish",
        "--output-dir", out_dir,
    ]
    if args.model:
        cmd += ["--model", args.model]
    # list-valued options last: each takes the words that follow it
    if args.tag:
        cmd += ["--tag"] + args.tag
    cmd += ["--allow-tools", "Write"]
    print("+ " + " ".join(cmd), flush=True)
    eval_rc = subprocess.call(cmd, cwd=REPO_ROOT)

    result_json = os.path.join(out_dir, "aggregate-result.json")
    if not os.path.isfile(result_json):
        print(f"run_plugin_evals: no {result_json} (exit code {eval_rc})", file=sys.stderr)
        return 2 if eval_rc in (0, 2) else eval_rc
    _, failures = check_results(result_json, args.keep)
    if eval_rc == 2:
        return 2
    return 1 if (eval_rc != 0 or failures) else 0


if __name__ == "__main__":
    sys.exit(main())
