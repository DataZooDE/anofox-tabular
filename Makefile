PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# VCPKG setup: Set the VCPKG_TOOLCHAIN_PATH variable that the included makefile expects.
ifeq ($(VCPKG_ROOT),)
	export VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)/vcpkg_installed/$(VCPKG_TARGET_TRIPLET)/share/vcpkg/scripts/buildsystems/vcpkg.cmake
else
	export VCPKG_TOOLCHAIN_PATH ?= $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
endif

# Configuration of extension
EXT_NAME=anofox_tabular
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Force unified "build" directory regardless of generator (ninja or make)
# This simplifies .gitignore and documentation by avoiding build-ninja vs build split
BUILD_ROOT:=build

# Override configure_ci to build libpostal from source with -fPIC
# System packages (Alpine libpostal-dev) lack -fPIC, which is required
# for linking into shared libraries (DuckDB extensions)
configure_ci:
	bash $(PROJ_DIR)/scripts/install-deps-ci.sh

# Override test targets to disable telemetry during test runs
# This prevents local tests and CI/CD from polluting PostHog telemetry data
test_release_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/release/test/unittest "test/*"

test_debug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/debug/test/unittest "test/*"

test_reldebug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 ./build/reldebug/test/unittest "test/*"

# ── Smoke test ─────────────────────────────────────────────────────────────────
# Downloads the real DuckDB CLI for the version the extension was built against,
# then loads the built extension artifact and runs basic assertions.
# This verifies the artifact works for a real user, not just the internal test runner.
#
# Override the DuckDB version: make smoke_test DUCKDB_VERSION=v1.4.4
DUCKDB_VERSION ?= $(shell git -C $(PROJ_DIR)duckdb describe --tags --exact-match 2>/dev/null)

.PHONY: smoke_test smoke_test_debug

# Depend on the artifact file, not the full 'release' target, because the
# loadable extension is built independently of test/unittest and libduckdb.so.
# The script validates the artifact's presence and prints a clear error if missing.
SMOKE_EXT_RELEASE := $(PROJ_DIR)build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension
SMOKE_EXT_DEBUG   := $(PROJ_DIR)build/debug/extension/anofox_tabular/anofox_tabular.duckdb_extension

smoke_test: $(SMOKE_EXT_RELEASE)
	bash $(PROJ_DIR)test/smoke/smoke_test.sh \
		"$(SMOKE_EXT_RELEASE)" \
		"$(DUCKDB_VERSION)"

smoke_test_debug: $(SMOKE_EXT_DEBUG)
	bash $(PROJ_DIR)test/smoke/smoke_test.sh \
		"$(SMOKE_EXT_DEBUG)" \
		"$(DUCKDB_VERSION)"
