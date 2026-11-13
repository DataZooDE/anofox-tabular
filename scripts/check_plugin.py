#!/usr/bin/env python3
"""scripts/check_plugin.py

No-build consistency check of the Claude Code plugin (plugins/anofox-tabular/), run by
the Static audits workflow (.github/workflows/audit.yml) and by `make check-plugin`.

Fails when:
- .claude-plugin/marketplace.json or plugins/anofox-tabular/.claude-plugin/plugin.json
  does not parse, or the marketplace entry's name/description/source disagree with
  plugin.json;
- plugin.json `version` (vYYYY.MM.DD without the v) differs from the Python package
  version in python/pyproject.toml (YYYY.M.D) -- both are bumped in the release PR;
- a skill's SKILL.md has no frontmatter, its `name` is not its directory name, its
  `description` is empty or has no "Use when" sentence, or its `version` differs from
  plugin.json;
- a skill is not listed in README.md, CLAUDE.md and AGENTS.md.

The SQL inside the skills is checked by scripts/check_doc_snippets.py (needs a build).
"""
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGIN_DIR = os.path.join(REPO_ROOT, "plugins", "anofox-tabular")
SKILLS_DIR = os.path.join(PLUGIN_DIR, "skills")
MARKETPLACE = os.path.join(REPO_ROOT, ".claude-plugin", "marketplace.json")
PLUGIN_JSON = os.path.join(PLUGIN_DIR, ".claude-plugin", "plugin.json")
PYPROJECT = os.path.join(REPO_ROOT, "python", "pyproject.toml")
LISTINGS = ["README.md", "CLAUDE.md", "AGENTS.md"]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def calver_key(version):
    """'2026.11.02' and '2026.11.2' -> (2026, 11, 2)."""
    m = re.fullmatch(r"v?(\d{4})\.(\d{1,2})\.(\d{1,2})", version.strip())
    return tuple(int(x) for x in m.groups()) if m else None


def parse_frontmatter(text):
    """Minimal YAML subset: `key: value` and folded `key: >` blocks."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None
    fields, key = {}, None
    for line in lines[1:]:
        if line.strip() == "---":
            return {k: " ".join(v).strip() for k, v in fields.items()}
        m = re.match(r"^([A-Za-z][A-Za-z0-9_-]*):\s*(.*)$", line)
        if m:
            key = m.group(1)
            value = m.group(2).strip()
            fields[key] = [] if value in (">", "|", ">-", "|-") else [value.strip("'\"")]
        elif key is not None and line.startswith((" ", "\t")):
            fields[key].append(line.strip())
    return None


def main():
    errors = []
    try:
        marketplace = json.loads(read(MARKETPLACE))
        plugin = json.loads(read(PLUGIN_JSON))
    except (OSError, ValueError) as e:
        print(f"check_plugin: cannot read the manifests: {e}")
        return 1

    entries = [p for p in marketplace.get("plugins", []) if p.get("name") == plugin.get("name")]
    if len(entries) != 1:
        errors.append(f"marketplace.json has no single entry named {plugin.get('name')!r}")
    else:
        entry = entries[0]
        if entry.get("source") != "./plugins/anofox-tabular":
            errors.append(f"marketplace.json source is {entry.get('source')!r}, want './plugins/anofox-tabular'")
        if entry.get("description") != plugin.get("description"):
            errors.append("marketplace.json and plugin.json descriptions differ")

    version = plugin.get("version", "")
    if calver_key(version) is None:
        errors.append(f"plugin.json version {version!r} is not YYYY.MM.DD")
    m = re.search(r'^version\s*=\s*"([^"]+)"', read(PYPROJECT), re.MULTILINE)
    if not m or calver_key(m.group(1)) != calver_key(version):
        errors.append(f"plugin.json version {version!r} differs from python/pyproject.toml "
                      f"{m.group(1) if m else '?'!r}; bump both in the release PR")

    skills = sorted(d for d in os.listdir(SKILLS_DIR) if os.path.isdir(os.path.join(SKILLS_DIR, d)))
    if not skills:
        errors.append("no skills under plugins/anofox-tabular/skills/")
    listings = {name: read(os.path.join(REPO_ROOT, name)) for name in LISTINGS}
    for skill in skills:
        path = os.path.join(SKILLS_DIR, skill, "SKILL.md")
        rel = os.path.relpath(path, REPO_ROOT)
        if not os.path.exists(path):
            errors.append(f"{rel} is missing")
            continue
        fm = parse_frontmatter(read(path))
        if fm is None:
            errors.append(f"{rel}: no frontmatter between --- lines")
            continue
        if fm.get("name") != skill:
            errors.append(f"{rel}: name {fm.get('name')!r} is not the directory name {skill!r}")
        description = fm.get("description", "")
        if not description:
            errors.append(f"{rel}: empty description")
        elif "Use when" not in description:
            errors.append(f"{rel}: description has no 'Use when ...' sentence")
        if fm.get("version") != version:
            errors.append(f"{rel}: version {fm.get('version')!r} differs from plugin.json {version!r}")
        for name, text in listings.items():
            if f"`{skill}`" not in text:
                errors.append(f"{name} does not list the skill `{skill}`")

    for e in errors:
        print(f"check_plugin: {e}")
    print(f"check_plugin: {len(skills)} skills, {len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
