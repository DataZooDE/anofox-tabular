#!/usr/bin/env bash
# Compile src/include/anofox_duckdb_vectors.hpp with stray DuckDB headers on the
# include path, the way a machine with a system DuckDB package (/usr/include/duckdb)
# sees it. Ported from anofox-forecast (DataZooDE/anofox-forecast#312).
#
# A version probe such as __has_include("duckdb/common/vector/list_vector.hpp")
# asks whether a header exists ANYWHERE on the include path. When the DuckDB being
# built against lacks it (v1.5.6 has no duckdb/common/vector/), the compiler keeps
# searching and finds a copy from another DuckDB version in the system include
# directories, e.g. a system DuckDB 2.0. This script plants a stub that fails with
# #error for every header that either (a) another supported DuckDB line has and the
# one at DUCKDB_INCLUDE lacks, or (b) the sources probe with __has_include and the
# one at DUCKDB_INCLUDE lacks. The stubs go on -isystem, which is searched after the
# DuckDB tree (-I) and before the real system directories, so a stub also wins over
# a system DuckDB on a developer machine. A header that is found there fails the
# compile.
#
# The bundled ./duckdb is v2.0.0, which has every header the sources ever probed,
# so the check only tests something against a v1.5.6 tree (CI clones one):
#   DUCKDB_INCLUDE=<duckdb v1.5.6>/src/include scripts/check_compat_stray_headers.sh
# (CXX and DUCKDB_INCLUDE override; DUCKDB_INCLUDE defaults to ./duckdb/src/include,
# which is v2.0.0 and plants nothing, so pass a v1.5.6 tree as CI does)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
duckdb_include=${DUCKDB_INCLUDE:-$root/duckdb/src/include}
cxx=${CXX:-c++}

if [ ! -f "$duckdb_include/duckdb.hpp" ]; then
	echo "no DuckDB headers at $duckdb_include (git submodule update --init duckdb)" >&2
	exit 2
fi

stray=$(mktemp -d)
trap 'rm -rf "$stray"' EXIT

# (a) Headers of DuckDB 2.0 that 1.5.x lacks and that the sources once probed for
#     (2.0 split the vector helpers out of vector.hpp).
known="duckdb/common/vector/list_vector.hpp
duckdb/common/vector/struct_vector.hpp"
# (b) Every header the sources probe for.
probed=$(grep -rhoE '__has_include[[:space:]]*\([[:space:]]*["<]duckdb[^">]*[">]' "$root/src" |
	sed -E 's/.*["<](duckdb[^">]*)[">]$/\1/' || true)

planted=0
for header in $(printf '%s\n' $known $probed | sort -u); do
	if [ ! -e "$duckdb_include/$header" ]; then
		mkdir -p "$stray/$(dirname "$header")"
		printf '#error "stray DuckDB header %s: not in the DuckDB built against, found on a later include path"\n' \
			"$header" >"$stray/$header"
		echo "planted stray $header"
		planted=$((planted + 1))
	fi
done

# With nothing planted the compile below tests nothing, so that is a failure: point
# DUCKDB_INCLUDE at a DuckDB tree that lacks the probed headers (e.g. v1.5.6).
if [ "$planted" -eq 0 ] && [ "${ALLOW_NO_STRAY:-0}" != 1 ]; then
	echo "error: no stray headers planted, so this check would test nothing; set DUCKDB_INCLUDE to a DuckDB v1.5.6 tree (or ALLOW_NO_STRAY=1)" >&2
	exit 1
fi

# The TU lives outside $stray: a quoted include searches the includer's directory first.
tu=$(mktemp -d)
trap 'rm -rf "$stray" "$tu"' EXIT
printf '#include "anofox_duckdb_vectors.hpp"\n' >"$tu/tu.cpp"
"$cxx" -std=c++17 -fsyntax-only -I"$duckdb_include" -I"$root/src/include" -isystem "$stray" "$tu/tu.cpp"
echo "anofox_duckdb_vectors.hpp compiles with $planted stray header(s) on the include path"
