#!/usr/bin/env python3
"""scripts/check_doc_snippets.py

Docs/skills SQL snippet + function-name checker for anofox_tabular (ported from
anofox-forecast's scripts/check_doc_snippets.py).

Usage:
    python3 scripts/check_doc_snippets.py [FILE ...]
    python3 scripts/check_doc_snippets.py --changed-since REF
    python3 scripts/check_doc_snippets.py --out PATH [FILE ...]
    python3 scripts/check_doc_snippets.py --self-test

Default scan (no FILE args, no --changed-since): README.md, every docs/**/*.md (except
docs/reviews/) and every plugins/anofox-tabular/skills/**/*.md -- every ```sql fence in
each is executed. Positional FILE args restrict the scan to exactly those files;
`--changed-since REF` selects the files changed relative to REF.

Each ```sql block runs in the shell of the local build (build/<ANOFOX_BUILD or
release>/duckdb) in an in-memory database, with the extension loaded from its file
(`-unsigned`, as scripts/duckdb-dev.sh does: DuckDB 2.0's shell does not link the
extension statically), telemetry off (DATAZOO_DISABLE_TELEMETRY=1), traces at `warn`,
`autoinstall_known_extensions = false`, `anofox_ner_auto_download = false` and a fresh
temporary directory as both cwd and HOME, so nothing is downloaded. With
ANOFOX_NER_AVAILABLE=1 the real HOME (where the NER model is cached) is kept and the
download stays on. A block runs on its own, after scripts/doc_snippets/fixtures.sql and after the
earlier setup blocks (CREATE TABLE/VIEW/MACRO, INSERT) of the same file that it
references (transitively).

Status per block:
  PASS / FAIL
  EXPECTED-ERROR  the next fence is a ```text block whose first line is an error
                  (`Binder Error: ...`): the block must raise that error, its first line
                  matching the documented one (either may be cut short). Or the block is
                  keyed in scripts/doc_snippets/expected_errors.txt and must fail. A
                  block that shows an error sets up later blocks with the statements
                  before its last one.
  SYNOPSIS        keyed in scripts/doc_snippets/synopsis.txt -- a signature or
                  placeholder, never executed
  SKIP-NETWORK    INSTALL or an http(s)/s3 URL (never run); email validation in the
                  'dns'/'smtp' mode or a block keyed `network` in requires.txt (run
                  with ANOFOX_DOCS_NETWORK=1)
  NO-RUN          a ```sql no-run fence (install commands, file paths of your machine)
  SKIP-ENV        a block that needs libpostal data (any postal_* call, or keyed
                  `postal` in requires.txt; run with POSTAL_AVAILABLE=1) or the NER
                  model (keyed `ner`; run with ANOFOX_NER_AVAILABLE=1)
  SKIP-VERSION    first line `-- DuckDB 2.0+` and the shell is older than 2.0

Function names: every `anofox_*` token in a backtick code span, and every token used
in call syntax (`name(`) whose first `_`-segment is the first segment of an
anofox_tabular function or alias (email_, phonenumber_, pii_, diff_, ...), in any
scanned markdown body and in a SKILL.md frontmatter description, must be a registered
function or setting (duckdb_functions(), duckdb_settings()), a macro the same file
creates, or listed in scripts/doc_snippets/allowed_names.txt (postal names are not
checked on a build without libpostal, which registers none). Otherwise it is a FAIL
("unknown function name <name>"). Tokens inside ```python/```text/... fences are not
checked; tokens inside ```sql fences are (the execution checks them too).

On a full scan, keys in expected_errors.txt / synopsis.txt / requires.txt whose block no
longer exists, and allowed_names.txt entries that are registered or no longer mentioned,
are STALE rows.

Writes a TSV (status, file, line, first SQL line, detail) to --out PATH (default
build/doc-snippets.tsv) and prints a summary line. Exit codes: 0 clean; 1 on any FAIL or
STALE; 2 if the shell or extension is missing or the function-name query fails.

Needs a build (`GEN=ninja make release`); `make check-docs` runs it, and the DuckDB 2.0
CI workflow (.github/workflows/Next20Tests.yml) runs it after the SQL tests.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

SUPPORT_DIR = os.path.join(SCRIPT_DIR, "doc_snippets")
FIXTURES_FILE = os.path.join(SUPPORT_DIR, "fixtures.sql")
EXPECTED_ERRORS_FILE = os.path.join(SUPPORT_DIR, "expected_errors.txt")
SYNOPSIS_FILE = os.path.join(SUPPORT_DIR, "synopsis.txt")
REQUIRES_FILE = os.path.join(SUPPORT_DIR, "requires.txt")
ALLOWED_NAMES_FILE = os.path.join(SUPPORT_DIR, "allowed_names.txt")

BUILD = os.environ.get("ANOFOX_BUILD", "release")
DUCKDB_BIN = os.path.join(REPO_ROOT, "build", BUILD, "duckdb")
EXTENSION_FILE = os.path.join(
    REPO_ROOT, "build", BUILD, "extension", "anofox_tabular", "anofox_tabular.duckdb_extension"
)
DEFAULT_OUT = os.path.join(REPO_ROOT, "build", "doc-snippets.tsv")

DEFAULT_SCAN_DIRS = ["docs", os.path.join("plugins", "anofox-tabular", "skills")]
EXCLUDED_DIRS = [os.path.join("docs", "reviews")]

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)((?:[ \t]+[A-Za-z0-9_+-]+)*)[ \t]*$")
# the first line of a ```text block that shows the error a ```sql block raises
ERROR_OUTPUT_RE = re.compile(r"^[A-Z][A-Za-z ]* Error: ")
SETUP_RE = re.compile(
    r"\b(CREATE\s+(OR\s+REPLACE\s+)?(TEMP\s+|TEMPORARY\s+)?(TABLE|VIEW|MACRO)|INSERT)\b",
    re.IGNORECASE,
)
CREATE_NAME_RE = re.compile(
    r"\bCREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP\s+|TEMPORARY\s+)?(?:TABLE|VIEW|MACRO)\s+"
    r"(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)",
    re.IGNORECASE,
)
INSERT_NAME_RE = re.compile(r"\bINSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
PLACEHOLDER_ELLIPSIS_RE = re.compile(r"(?<!\d)\.\.\.(?!\d)")
NETWORK_RE = re.compile(r"\bINSTALL\b|https?://|s3://", re.IGNORECASE)
POSTAL_RE = re.compile(r"\b(?:anofox_tab_)?postal_[a-z0-9_]+\s*\(", re.IGNORECASE)
# email validation in the 'dns' or 'smtp' mode (also inside a quoted check expression)
EMAIL_NETWORK_RE = re.compile(r"\bemail_[a-z_]+\s*\(.*?'{1,2}(?:dns|smtp)'{1,2}", re.IGNORECASE | re.DOTALL)
POSTAL_NAME_RE = re.compile(r"^(?:anofox_tab_)?postal_")
POSTAL_SETTING_RE = re.compile(r"\banofox_tab_postal_[a-z0-9_]+\b(?!\s*\()", re.IGNORECASE)
V2_ONLY_RE = re.compile(r"\A\s*--\s*DuckDB 2\.0\+")
# A block's own LOAD of the extension by name: the extension is already loaded from
# its file, and by name DuckDB would look in the (empty, temporary) extension dir.
LOAD_SELF_RE = re.compile(r"^\s*LOAD\s+'?anofox_tabular'?\s*;\s*$", re.IGNORECASE | re.MULTILINE)

# `anofox_x` or `anofox_x(...)`, not file names (`anofox_email.cpp`) or prefixes (`anofox_tab_*`)
ANOFOX_BACKTICK_RE = re.compile(r"`(anofox_[a-z0-9_]*[a-z0-9])(?=`|\()")
CALL_RE = re.compile(r"(?<![A-Za-z0-9_.])([a-z][a-z0-9]*(?:_[a-z0-9]+)+)\s*\(")

REQUIRE_KINDS = {
    "postal": ("SKIP-ENV", "POSTAL_AVAILABLE"),
    "ner": ("SKIP-ENV", "ANOFOX_NER_AVAILABLE"),
    "network": ("SKIP-NETWORK", "ANOFOX_DOCS_NETWORK"),
}
STATUSES = ["PASS", "FAIL", "EXPECTED-ERROR", "SYNOPSIS", "NO-RUN", "SKIP-NETWORK", "SKIP-ENV", "SKIP-VERSION", "STALE"]


def read_text(abs_path):
    with open(abs_path, encoding="utf-8") as f:
        return f.read()


def is_excluded(rel_path):
    return any(rel_path == d or rel_path.startswith(d + os.sep) for d in EXCLUDED_DIRS)


def changed_md_files(ref):
    result = subprocess.run(
        ["git", "diff", "--name-only", ref, "--", "README.md"] + DEFAULT_SCAN_DIRS,
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    )
    files = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return [
        f for f in files
        if f.endswith(".md") and not is_excluded(f) and os.path.exists(os.path.join(REPO_ROOT, f))
    ]


def default_scan_files():
    files = []
    if os.path.exists(os.path.join(REPO_ROOT, "README.md")):
        files.append("README.md")
    for scan_dir in DEFAULT_SCAN_DIRS:
        abs_dir = os.path.join(REPO_ROOT, scan_dir)
        if not os.path.isdir(abs_dir):
            continue
        for root, dirs, fnames in os.walk(abs_dir):
            dirs.sort()
            for fn in sorted(fnames):
                rel = os.path.relpath(os.path.join(root, fn), REPO_ROOT)
                if fn.endswith(".md") and not is_excluded(rel):
                    files.append(rel)
    return files


def iter_fences(text):
    """Yield (language, attributes, body_text, start_line_idx, end_line_idx) for every
    fenced block; attributes are the words after the language (```sql no-run)."""
    lines = text.splitlines()
    i, n = 0, len(lines)
    while i < n:
        m = FENCE_RE.match(lines[i].strip())
        if m:
            lang = m.group(1).lower()
            attrs = set(m.group(2).split())
            start = i
            body = []
            i += 1
            while i < n and lines[i].strip() != "```":
                body.append(lines[i])
                i += 1
            yield lang, attrs, "\n".join(body), start, i
        i += 1


def extract_sql_blocks(text):
    """Return list of (block_text, start_line_idx, attributes, expected_error) for ```sql
    fenced blocks. expected_error is the first line of the next fence when that fence is a
    ```text block showing an error (`Binder Error: ...`), else None."""
    fences = list(iter_fences(text or ""))
    blocks = []
    for idx, (lang, attrs, body, start, _) in enumerate(fences):
        if lang != "sql":
            continue
        expected_error = None
        if idx + 1 < len(fences) and fences[idx + 1][0] == "text":
            first = next((ln.strip() for ln in fences[idx + 1][2].splitlines() if ln.strip()), "")
            if ERROR_OUTPUT_RE.match(first):
                expected_error = first
        blocks.append((body, start, attrs, expected_error))
    return blocks


def normalize_ws(text):
    return " ".join(text.split())


def error_matches(expected, actual):
    """The documented error line and the raised one agree; either may be cut short."""
    e, a = normalize_ws(expected), normalize_ws(actual)
    return bool(e) and bool(a) and (a.startswith(e) or e.startswith(a))


def first_non_comment_line(block_text):
    for line in block_text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("--"):
            return stripped
    for line in block_text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def load_key_set(path):
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        return {line.rstrip("\n") for line in f if line.strip() and not line.startswith("#")}


def load_requires(path):
    """file<TAB>first line<TAB>kind -> {key: kind}."""
    requires = {}
    if not os.path.exists(path):
        return requires
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3 or parts[2] not in REQUIRE_KINDS:
                print(f"ERROR: bad requires.txt line (want file<TAB>first line<TAB>postal|ner|network): {line}",
                      file=sys.stderr)
                sys.exit(2)
            requires[f"{parts[0]}\t{parts[1]}"] = parts[2]
    return requires


def load_allowed_names(path):
    """name<TAB>reason, one per line; blank lines and '#'-comment lines ignored."""
    names = {}
    if not os.path.exists(path):
        return names
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            parts = line.split("\t", 1)
            if parts[0].strip():
                names[parts[0].strip()] = parts[1].strip() if len(parts) > 1 else ""
    return names


def strip_sql_comments(text):
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


TRANSACTION_RE = re.compile(r"^(?:BEGIN|START\s+TRANSACTION)\b", re.IGNORECASE)
STATEMENT_END_RE = re.compile(r";[ \t]*(?:--[^\n]*)?(?:\n|\Z)")


def setup_part_of_error_block(block_text):
    """The statements before the last one: in a block that shows an error, the last
    statement raises it and the ones before set up its data. A block that opens a
    transaction (BEGIN ... error ... ROLLBACK) keeps only the statements before BEGIN."""
    statements = [st for st in STATEMENT_END_RE.split(block_text) if strip_sql_comments(st).strip()]
    for i, st in enumerate(statements):
        if TRANSACTION_RE.match(strip_sql_comments(st).strip()):
            statements = statements[:i] + ["-"]  # the placeholder stands for the failing part
            break
    return ";\n".join(statements[:-1]) + ";" if len(statements) > 1 else ""


def setup_created_names(block_text):
    text = strip_sql_comments(block_text)
    return set(CREATE_NAME_RE.findall(text)) | set(INSERT_NAME_RE.findall(text))


def self_created_names(text):
    return {n for n in CREATE_NAME_RE.findall(text) if n}


def select_required_setup(target_text, setup_accum, fixture_names=()):
    """Transitive closure of setup blocks (in doc order) referenced by target_text,
    excluding names the target creates itself and names fixtures.sql provides."""
    shadowed = self_created_names(strip_sql_comments(target_text)) | set(fixture_names)
    needed_text = [strip_sql_comments(target_text)]
    included = []
    included_names = set()
    changed = True
    while changed:
        changed = False
        combined = "\n".join(needed_text)
        for names, block_text in setup_accum:
            if id(block_text) in included_names or not (names - shadowed):
                continue
            if any(re.search(r"\b" + re.escape(name) + r"\b", combined) for name in names - shadowed):
                included.append(block_text)
                included_names.add(id(block_text))
                needed_text.append(strip_sql_comments(block_text))
                changed = True
    order = {id(bt): i for i, (_, bt) in enumerate(setup_accum)}
    included.sort(key=lambda bt: order.get(id(bt), 0))
    return included


def first_error_line(stderr, stdout):
    for stream in (stderr, stdout):
        for line in stream.splitlines():
            stripped = line.strip()
            # skip the extension's trace lines, the error is what follows them
            if stripped and not stripped.startswith("[anofox]") and not stripped.startswith("["):
                return stripped[:200]
    return ""


_SHELL_ARGS = None


def shell_args():
    """-no-agent on 2.0 (terse agent output when stdout is not a terminal), not on 1.5."""
    global _SHELL_ARGS
    if _SHELL_ARGS is None:
        args = [DUCKDB_BIN, "-unsigned"]
        probe = subprocess.run([DUCKDB_BIN, "-no-agent", "-c", "SELECT 1"], capture_output=True, text=True)
        if probe.returncode == 0:
            args.append("-no-agent")
        _SHELL_ARGS = args + ["-bail", "-list", ":memory:"]
    return _SHELL_ARGS


def run_block_safe(sql_text, timeout=120):
    """Execute sql_text in :memory: with the extension loaded, a fresh temp dir as cwd
    and HOME (removed afterward), autoinstall disabled, telemetry off."""
    tmp_dir = tempfile.mkdtemp(prefix="doc_snippets_")
    try:
        env = dict(os.environ)
        env["DATAZOO_DISABLE_TELEMETRY"] = "1"
        prelude = (
            "SET autoinstall_known_extensions = false;\n"
            f"LOAD '{EXTENSION_FILE}';\n"
            "SET anofox_tab_trace_level = 'warn';\n"
        )
        if env_enabled("ANOFOX_NER_AVAILABLE"):
            pass  # keep HOME: the NER model is cached there
        else:
            # offline: never download the NER model (178 MB) into the temporary HOME
            env["HOME"] = tmp_dir
            prelude += "SET anofox_ner_auto_download = false;\n"
        try:
            result = subprocess.run(
                shell_args(), input=prelude + LOAD_SELF_RE.sub("", sql_text), cwd=tmp_dir,
                capture_output=True, text=True, timeout=timeout, env=env,
            )
            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return 124, "", f"TIMEOUT after {timeout}s"
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def query_registered_names():
    """All function and setting names, and the anofox_tabular name prefixes."""
    rc, out, err = run_block_safe(
        "SELECT DISTINCT function_name FROM duckdb_functions()\n"
        "UNION SELECT name FROM duckdb_settings();\n"
    )
    if rc != 0:
        print(f"ERROR: failed to query the registered names: {(err.strip() or out.strip())[:500]}", file=sys.stderr)
        sys.exit(2)
    names = {line.strip() for line in out.splitlines() if line.strip()}
    return names, tabular_prefixes(names)


def tabular_prefixes(names):
    """First `_`-segment of every anofox_tab_* function and of its short alias."""
    prefixes = {"anofox"}
    for name in names:
        if name.startswith("anofox_tab_"):
            alias = name[len("anofox_tab_"):]
            if alias in names and "_" in alias:
                prefixes.add(alias.split("_", 1)[0])
    return prefixes


def collect_name_mentions(text, prefixes):
    """(name, line_no) mentions to resolve: `anofox_*` code spans and anofox_tabular-
    prefixed call syntax, outside non-SQL fences."""
    lines = text.splitlines()
    skip = set()
    for lang, _, _, start, end in iter_fences(text):
        if lang != "sql":
            skip.update(range(start, end + 1))
    mentions = []
    for i, line in enumerate(lines):
        if i in skip:
            continue
        for m in ANOFOX_BACKTICK_RE.finditer(line):
            mentions.append((m.group(1), i + 1))
        for m in CALL_RE.finditer(line):
            name = m.group(1)
            if name.split("_", 1)[0] in prefixes and not name.startswith("anofox_"):
                mentions.append((name, i + 1))
            elif name.startswith("anofox_"):
                mentions.append((name, i + 1))
    return mentions


def env_enabled(var):
    return os.environ.get(var, "") not in ("", "0")


def classify_skip(key, block_text, requires, synopsis_keys, duckdb_major, attrs=frozenset()):
    """Return (status, detail) if the block is not executed, else None."""
    if "no-run" in attrs:
        return "NO-RUN", "```sql no-run block, not executed"
    if NETWORK_RE.search(block_text):
        return "SKIP-NETWORK", "INSTALL or URL in the block, never executed"
    if V2_ONLY_RE.search(block_text) and duckdb_major < 2:
        return "SKIP-VERSION", "DuckDB 2.0+ block, the shell is older"
    if key in synopsis_keys:
        return "SYNOPSIS", "signature or placeholder, not executed"
    kind = requires.get(key)
    code = strip_sql_comments(block_text)
    if kind is None and POSTAL_RE.search(code):
        kind = "postal"
    if kind is None and EMAIL_NETWORK_RE.search(code):
        kind = "network"
    if kind is not None:
        status, var = REQUIRE_KINDS[kind]
        if not env_enabled(var):
            return status, f"needs {kind} (set {var}=1 to run)"
    return None


def self_test():
    md = (
        "---\nname: x\ndescription: >\n  Uses `anofox_tab_email_is_valid` and email_validate(...).\n---\n"
        "Call `email_is_valid('a@b.de')` or email_bogus(1).\n"
        "```python\nemail_ignored(1)\n```\n"
        "```sql\nCREATE TABLE t AS SELECT 1 AS a;\n```\n"
        "```sql\nSELECT * FROM t;\n```\n"
        "```sql\nSELECT postal_parse_address('x');\n```\n"
    )
    names = {"anofox_tab_email_is_valid", "email_is_valid", "anofox_tab_email_validate", "email_validate",
             "anofox_tab_postal_parse_address", "postal_parse_address"}
    prefixes = tabular_prefixes(names)
    assert prefixes == {"anofox", "email", "postal"}, prefixes
    mentioned = [n for n, _ in collect_name_mentions(md, prefixes)]
    assert "anofox_tab_email_is_valid" in mentioned and "email_validate" in mentioned, mentioned
    assert "email_bogus" in mentioned and "email_is_valid" in mentioned, mentioned
    assert "email_ignored" not in mentioned, mentioned
    blocks = extract_sql_blocks(md)
    assert len(blocks) == 3, blocks
    setup = [(setup_created_names(blocks[0][0]), blocks[0][0])]
    assert select_required_setup(blocks[1][0], setup) == [blocks[0][0]]
    assert select_required_setup(blocks[0][0], setup) == []
    key = f"SKILL.md\t{first_non_comment_line(blocks[2][0])}"
    def status_of(*a):
        return (classify_skip(*a) or (None,))[0]

    saved = os.environ.pop("POSTAL_AVAILABLE", None)
    try:
        assert status_of(key, blocks[2][0], {}, set(), 2) == "SKIP-ENV"
        assert status_of(key, blocks[2][0], {}, {key}, 2) == "SYNOPSIS"
        assert status_of("k", "INSTALL x;", {}, set(), 2) == "SKIP-NETWORK"
        assert status_of("k", "SELECT 1;", {"k": "ner"}, set(), 2) == "SKIP-ENV"
        assert classify_skip("k", "SELECT 1;", {}, set(), 2) is None
        assert status_of("k", "-- DuckDB 2.0+\nSELECT 1;", {}, set(), 1) == "SKIP-VERSION"
    finally:
        if saved is not None:
            os.environ["POSTAL_AVAILABLE"] = saved
    err_md = "```sql no-run\nINSTALL x;\n```\n```sql\nSELECT 1/0;\n```\n\n```text\nBinder Error: boom\n```\n"
    err_blocks = extract_sql_blocks(err_md)
    assert err_blocks[0][2] == {"no-run"} and err_blocks[0][3] is None, err_blocks
    assert err_blocks[1][3] == "Binder Error: boom", err_blocks
    assert status_of("k", "SELECT 1;", {}, set(), 2, {"no-run"}) == "NO-RUN"
    assert error_matches("Binder Error: boom", "Binder Error:  boom at line 1")
    assert not error_matches("Binder Error: boom", "Catalog Error: boom")
    assert setup_created_names("CREATE TABLE a AS SELECT 1;\nCREATE VIEW b AS SELECT 2;") == {"a", "b"}
    assert setup_part_of_error_block("CREATE TABLE a (x INT);\nINSERT INTO a VALUES (1);\n\nSELECT boom(x) FROM a;") \
        == "CREATE TABLE a (x INT);\nINSERT INTO a VALUES (1);"
    assert setup_part_of_error_block("SELECT boom();") == ""
    assert setup_part_of_error_block("CREATE TABLE a (x INT);\nBEGIN;\nINSERT INTO a VALUES (1);\nSELECT boom();\nROLLBACK;") \
        == "CREATE TABLE a (x INT);"
    assert setup_part_of_error_block("BEGIN;\nINSERT INTO a VALUES (1);\nSELECT boom();") == ""
    assert status_of("k", "SELECT email_validate('a@b.de', 'dns');", {}, set(), 2) == "SKIP-NETWORK"
    assert status_of("k", "FROM compliance('c', 'email_is_valid(email, ''smtp'')');", {}, set(), 2) == "SKIP-NETWORK"
    assert status_of("k", "SELECT email_validate('a@b.de', 'regex');", {}, set(), 2) is None
    assert LOAD_SELF_RE.sub("", "LOAD anofox_tabular;\nSELECT 1;") == "\nSELECT 1;"
    print("check_doc_snippets: self-test passed")
    return 0


def main():
    args = sys.argv[1:]
    if args == ["--self-test"]:
        return self_test()
    files_arg, changed_since, out_path = [], None, DEFAULT_OUT
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--changed-since", "--out"):
            i += 1
            if i >= len(args):
                print(f"ERROR: {a} requires an argument", file=sys.stderr)
                return 1
            if a == "--changed-since":
                changed_since = args[i]
            else:
                out_path = args[i]
        else:
            files_arg.append(a)
        i += 1

    for path, what in ((DUCKDB_BIN, "duckdb shell"), (EXTENSION_FILE, "extension")):
        if not os.path.exists(path):
            print(f"ERROR: {what} not found at {path} -- build it first (GEN=ninja make release)", file=sys.stderr)
            return 2

    is_full_scan = not files_arg and not changed_since
    if files_arg:
        files = files_arg
    elif changed_since:
        files = changed_md_files(changed_since)
    else:
        files = default_scan_files()

    fixtures = read_text(FIXTURES_FILE) if os.path.exists(FIXTURES_FILE) else ""
    fixture_names = set(CREATE_NAME_RE.findall(strip_sql_comments(fixtures)))
    expected_errors = load_key_set(EXPECTED_ERRORS_FILE)
    synopsis_keys = load_key_set(SYNOPSIS_FILE)
    requires = load_requires(REQUIRES_FILE)
    allowed_names = load_allowed_names(ALLOWED_NAMES_FILE)
    registered_names, prefixes = query_registered_names()
    # a build without libpostal (macOS, Windows, ANOFOX_TABULAR_NO_LIBPOSTAL=1) registers no
    # postal function or setting: their names cannot be checked there
    postal_built = any(n.startswith("anofox_tab_postal") for n in registered_names)
    rc, out, _ = run_block_safe("SELECT version();\n")
    m = re.search(r"v(\d+)\.", out or "")
    duckdb_major = int(m.group(1)) if rc == 0 and m else 0

    rows = []  # [status, file, line, first SQL line, detail]; status filled in by run_job
    jobs = []
    counts = {s: 0 for s in STATUSES}
    all_block_keys, all_mentioned_names, seen_unknown = set(), set(), set()

    for rel_path in files:
        abs_path = os.path.join(REPO_ROOT, rel_path)
        if not os.path.exists(abs_path):
            continue
        text = read_text(abs_path)
        own_macros = self_created_names(text)

        for name, line_no in collect_name_mentions(text, prefixes):
            all_mentioned_names.add(name)
            if name in registered_names or name in allowed_names or name in own_macros:
                continue
            if not postal_built and POSTAL_NAME_RE.match(name):
                continue
            if (rel_path, name) in seen_unknown:
                continue
            seen_unknown.add((rel_path, name))
            rows.append(["FAIL", rel_path, str(line_no), "", f"unknown function name {name}"])

        setup_accum = []
        for block_text, start_line, attrs, shown_error in extract_sql_blocks(text):
            first_line = first_non_comment_line(block_text)
            key = f"{rel_path}\t{first_line}"
            all_block_keys.add(key)
            is_placeholder = bool(PLACEHOLDER_ELLIPSIS_RE.search(block_text))
            skip = classify_skip(key, block_text, requires, synopsis_keys, duckdb_major, attrs)
            if not skip and not postal_built and POSTAL_SETTING_RE.search(strip_sql_comments(block_text)):
                # e.g. SET anofox_tab_postal_auto_download: the setting exists only in builds
                # with the postal module (not Windows, not ANOFOX_TABULAR_NO_LIBPOSTAL=1)
                skip = ("SKIP-ENV", "postal setting, this build has no postal module")
            row = [None, rel_path, str(start_line + 1), first_line[:100], ""]
            rows.append(row)
            if skip:
                row[0], row[4] = skip
            else:
                full_sql = fixtures
                for setup_sql in select_required_setup(block_text, setup_accum, fixture_names):
                    full_sql += "\n" + setup_sql
                full_sql += "\n" + block_text
                jobs.append((row, full_sql, key in expected_errors, shown_error))
            # a block that is meant to fail sets up later blocks with the statements
            # before the failing (last) one
            setup_text = block_text
            if key in expected_errors or shown_error is not None:
                setup_text = setup_part_of_error_block(block_text)
            if SETUP_RE.search(setup_text) and not is_placeholder and not skip:
                names = setup_created_names(setup_text)
                if names:
                    setup_accum.append((names, setup_text))

    def run_job(job):
        row, full_sql, listed_error, shown_error = job
        rc, stdout, stderr = run_block_safe(full_sql)
        actual = first_error_line(stderr, stdout)
        if rc == 0:
            if listed_error or shown_error:
                row[0], row[4] = "FAIL", "expected an error but the block passed"
            else:
                row[0] = "PASS"
        elif shown_error is not None:
            if error_matches(shown_error, actual):
                row[0] = "EXPECTED-ERROR"
            else:
                row[0], row[4] = "FAIL", f"the docs show '{shown_error[:90]}' but the block raised '{actual}'"
        elif listed_error:
            row[0] = "EXPECTED-ERROR"
        else:
            row[0], row[4] = "FAIL", actual

    workers = max(1, min(int(os.environ.get("CHECK_DOCS_JOBS", os.cpu_count() or 1)), 16))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(run_job, jobs))
    for row in rows:
        counts[row[0]] += 1

    if is_full_scan:
        for label, keys in (("expected_errors.txt", expected_errors), ("synopsis.txt", synopsis_keys),
                            ("requires.txt", set(requires))):
            for key in sorted(keys):
                if key not in all_block_keys:
                    rows.append(("STALE", key.split("\t", 1)[0], "", "", f"stale {label} entry: {key}"))
                    counts["STALE"] += 1
        for name in sorted(allowed_names):
            if name in registered_names:
                detail = f"{name} is a registered function now, drop it from the list"
            elif name not in all_mentioned_names:
                detail = f"{name} is no longer mentioned anywhere"
            else:
                continue
            rows.append(("STALE", "scripts/doc_snippets/allowed_names.txt", "", "", detail))
            counts["STALE"] += 1

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write("\t".join(row) + "\n")

    for row in rows:
        if row[0] in ("FAIL", "STALE"):
            print("\t".join(row))
    print("check_doc_snippets: " + ", ".join(f"{counts[s]} {s}" for s in STATUSES) + f" (details: {out_path})")
    return 1 if (counts["FAIL"] or counts["STALE"]) else 0


if __name__ == "__main__":
    sys.exit(main())
