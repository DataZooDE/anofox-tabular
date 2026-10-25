#!/usr/bin/env bash
# Start the DuckDB shell of the local build with anofox_tabular loaded.
#
# DuckDB 2.0's shell no longer links out-of-tree extensions statically, so the
# extension is loaded from its file (-unsigned). On v1.5 the same call works:
# the static extension is already there and the LOAD is a no-op.
#
# Usage: scripts/duckdb-dev.sh [duckdb shell arguments...]
#   scripts/duckdb-dev.sh -c "SELECT email_is_valid('a@b.com')"
#   ANOFOX_BUILD=debug scripts/duckdb-dev.sh mydb.duckdb
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${ANOFOX_BUILD:-release}"
shell="$root/build/$build/duckdb"
ext="$root/build/$build/extension/anofox_tabular/anofox_tabular.duckdb_extension"
args=(-unsigned -cmd "LOAD '$ext'")
# 2.0 switches to a terse "agent mode" when AI_AGENT is set and stdout is not a
# terminal; keep the normal output (v1.5 has no such flag)
if "$shell" -no-agent -c "SELECT 1" >/dev/null 2>&1; then
	args=(-no-agent "${args[@]}")
fi
exec "$shell" "${args[@]}" "$@"
