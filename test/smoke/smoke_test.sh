#!/usr/bin/env bash
# smoke_test.sh — Verify the anofox_tabular extension artifact using a real
# downloaded DuckDB CLI binary (not the one built alongside the extension).
#
# Usage:
#   ./test/smoke/smoke_test.sh [EXT_PATH [DUCKDB_VERSION]]
#
# Defaults:
#   EXT_PATH       = <project_root>/build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension
#   DUCKDB_VERSION = result of: git -C <project_root>/duckdb describe --tags --exact-match
#
# Environment:
#   DATAZOO_DISABLE_TELEMETRY=1 is always set (matches other test targets)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Arguments with defaults ───────────────────────────────────────────────────
EXT_PATH="${1:-}"
DUCKDB_VERSION="${2:-}"

if [[ -z "$EXT_PATH" ]]; then
    EXT_PATH="${PROJ_DIR}/build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension"
fi

# Resolve to absolute path
EXT_PATH="$(cd "$(dirname "$EXT_PATH")" && pwd)/$(basename "$EXT_PATH")"

if [[ -z "$DUCKDB_VERSION" ]]; then
    DUCKDB_VERSION="$(git -C "${PROJ_DIR}/duckdb" describe --tags --exact-match 2>/dev/null || true)"
    if [[ -z "$DUCKDB_VERSION" ]]; then
        echo "ERROR: Could not determine DuckDB version from git tag in ${PROJ_DIR}/duckdb." >&2
        echo "       Pass the version explicitly: ${BASH_SOURCE[0]} '' v1.5.2" >&2
        exit 1
    fi
fi

# ── Validate extension artifact ───────────────────────────────────────────────
if [[ ! -f "$EXT_PATH" ]]; then
    echo "ERROR: Extension artifact not found: ${EXT_PATH}" >&2
    echo "       Run 'make release' first." >&2
    exit 1
fi

# ── Platform detection ────────────────────────────────────────────────────────
OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux*)
        case "$ARCH" in
            x86_64)  CLI_ZIP="duckdb_cli-linux-amd64.zip" ;;
            aarch64) CLI_ZIP="duckdb_cli-linux-aarch64.zip" ;;
            *)
                echo "ERROR: Unsupported Linux architecture: ${ARCH}" >&2
                exit 1
                ;;
        esac
        ;;
    Darwin*)
        CLI_ZIP="duckdb_cli-osx-universal.zip"
        ;;
    *)
        echo "ERROR: Unsupported operating system: ${OS}" >&2
        exit 1
        ;;
esac

DOWNLOAD_URL="https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/${CLI_ZIP}"
CACHE_DIR="${PROJ_DIR}/.cache/smoke_test/duckdb-${DUCKDB_VERSION}"
DUCKDB_BIN="${CACHE_DIR}/duckdb"

# ── Print configuration ───────────────────────────────────────────────────────
echo "=== anofox_tabular smoke test ==="
echo "  DuckDB version : ${DUCKDB_VERSION}"
echo "  Platform       : ${OS}/${ARCH}"
echo "  Extension      : ${EXT_PATH}"
echo "  CLI cache      : ${DUCKDB_BIN}"
echo ""

# ── Download and cache DuckDB CLI ─────────────────────────────────────────────
if [[ -x "$DUCKDB_BIN" ]]; then
    echo "Using cached DuckDB CLI: ${DUCKDB_BIN}"
else
    echo "Downloading DuckDB CLI ${DUCKDB_VERSION} (${CLI_ZIP})..."
    echo "  URL: ${DOWNLOAD_URL}"
    mkdir -p "${CACHE_DIR}"

    ZIP_PATH="${CACHE_DIR}/${CLI_ZIP}"

    if command -v curl &>/dev/null; then
        curl -fsSL --retry 3 --retry-delay 2 -o "${ZIP_PATH}" "${DOWNLOAD_URL}"
    elif command -v wget &>/dev/null; then
        wget -q --tries=3 -O "${ZIP_PATH}" "${DOWNLOAD_URL}"
    else
        echo "ERROR: Neither curl nor wget is available." >&2
        exit 1
    fi

    echo "Extracting..."
    unzip -q -o "${ZIP_PATH}" -d "${CACHE_DIR}"
    rm -f "${ZIP_PATH}"

    if [[ ! -x "$DUCKDB_BIN" ]]; then
        echo "ERROR: Extraction succeeded but '${DUCKDB_BIN}' is missing or not executable." >&2
        echo "       Contents of ${CACHE_DIR}:" >&2
        ls -la "${CACHE_DIR}" >&2
        exit 1
    fi

    echo "DuckDB CLI ${DUCKDB_VERSION} ready."
fi

# ── Run smoke tests ───────────────────────────────────────────────────────────
SQL_FILE="${SCRIPT_DIR}/smoke_test.sql"

echo ""
echo "Running smoke tests..."
echo ""

set +e
{ echo "LOAD '${EXT_PATH}';"; cat "${SQL_FILE}"; } \
    | DATAZOO_DISABLE_TELEMETRY=1 "${DUCKDB_BIN}" -unsigned
EXIT_CODE=$?
set -e

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    echo "=== Smoke test PASSED ==="
else
    echo "=== Smoke test FAILED (exit code ${EXIT_CODE}) ===" >&2
    echo "" >&2
    echo "Diagnostic information:" >&2
    echo "  Extension path  : ${EXT_PATH}" >&2
    echo "  Extension type  : $(file "${EXT_PATH}" 2>/dev/null | cut -d: -f2- | xargs)" >&2
    echo "  DuckDB binary   : ${DUCKDB_BIN}" >&2
    echo "  DuckDB version  : ${DUCKDB_VERSION}" >&2
    echo "  Platform        : ${OS}/${ARCH}" >&2
    echo "  Download URL    : ${DOWNLOAD_URL}" >&2
    echo "" >&2
    echo "Common causes:" >&2
    echo "  - Extension was built for a different DuckDB version (try: make release)" >&2
    echo "  - Platform/architecture mismatch between extension and CLI binary" >&2
    exit 1
fi
