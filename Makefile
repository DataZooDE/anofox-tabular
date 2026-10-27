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

# DuckDB 2.0 takes its version from scripts/ci/release_version.txt instead of
# git, so a local build of the v2.0.0 tag is stamped as a dev version
# (v2.0.0-devNNNNN) that released DuckDB clients, e.g. the Python package,
# refuse to load. When ./duckdb sits exactly on a release tag, stamp that tag.
# An explicit OVERRIDE_GIT_DESCRIBE (CI) wins.
OVERRIDE_GIT_DESCRIBE ?= $(shell git -C $(PROJ_DIR)duckdb describe --tags --exact-match 2>/dev/null)

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Force unified "build" directory regardless of generator (ninja or make)
# This simplifies .gitignore and documentation by avoiding build-ninja vs build split
BUILD_ROOT:=build

# Patches applied to the DuckDB submodule before building (see duckdb_patches/).
# Currently EMPTY: the previous fmt stdext::checked_array_iterator removal
# (upstream duckdb/duckdb@0c19d698ca) is included upstream since v1.5.4, so
# no patch is needed. The mechanism is kept for future submodule patches.
# Idempotent: already-applied patches are detected and skipped; an empty
# duckdb_patches/ directory is a no-op (the unexpanded glob is skipped).
.PHONY: apply_duckdb_patches
apply_duckdb_patches:
	@for p in $(PROJ_DIR)duckdb_patches/*.patch; do \
		[ -e "$$p" ] || continue; \
		if git -C duckdb apply --reverse --check "$$p" 2>/dev/null; then \
			echo "DuckDB patch already applied: $$p"; \
		elif git -C duckdb apply --check "$$p" 2>/dev/null; then \
			git -C duckdb apply "$$p" && echo "Applied DuckDB patch: $$p"; \
		else \
			echo "ERROR: DuckDB patch does not apply: $$p" && exit 1; \
		fi; \
	done

release: apply_duckdb_patches
debug: apply_duckdb_patches
reldebug: apply_duckdb_patches

# Override configure_ci to build libpostal from source with -fPIC
# System packages (Alpine libpostal-dev) lack -fPIC, which is required
# for linking into shared libraries (DuckDB extensions)
configure_ci: apply_duckdb_patches
	bash $(PROJ_DIR)/scripts/install-deps-ci.sh

# The postal module is only compiled when libpostal is available (see
# CMakeLists.txt: static lib in /usr/local or /usr/lib, else pkg-config). On
# macOS/Windows it is absent, so postal functions and the
# anofox_tab_postal_data_path setting are not registered. Mirror that same
# detection here so postal tests run in Linux CI and skip elsewhere, via the
# POSTAL_AVAILABLE env var the postal sqllogictests guard on.
POSTAL_AVAILABLE := $(if $(or $(wildcard /usr/local/lib/libpostal.a),$(wildcard /usr/lib/libpostal.a),$(shell pkg-config --exists libpostal 2>/dev/null && echo 1)),1,)
POSTAL_TEST_ENV := $(if $(POSTAL_AVAILABLE),POSTAL_AVAILABLE=1,)

# Override test targets to disable telemetry during test runs
# This prevents local tests and CI/CD from polluting PostHog telemetry data
# DNNL_MAX_CPU_ISA=AVX2: limits OneDNN (used by OpenVINO) to AVX2 at most,
# preventing SIGILL in manylinux Docker containers that restrict AVX-512 CPUID
test_release_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 $(POSTAL_TEST_ENV) ./build/release/test/unittest "test/*"

test_debug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 $(POSTAL_TEST_ENV) ./build/debug/test/unittest "test/*"

test_reldebug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 $(POSTAL_TEST_ENV) ./build/reldebug/test/unittest "test/*"

# The NER sqllogictests guard on OPENVINO_AVAILABLE because they need an
# OpenVINO build and download the NER model from Hugging Face on first use
# (~66 MB, cached in ~/.duckdb/extensions/anofox/ner). The regular test
# targets skip them; this target runs them, and fails if the extension was
# built without OpenVINO rather than letting every NER assertion degrade.
# The tests of the multilingual models (test_ner_multi) also guard on
# ANOFOX_TEST_NER_MULTI: the model is chosen once per process, so each of them
# runs in a process of its own, and they download 135 MB and 178 MB.
NER_MULTI_TESTS := test/sql/anofox_ner_multilingual.test test/sql/anofox_ner_multilingual_distilbert.test
NER_TESTS := $(filter-out $(NER_MULTI_TESTS),$(shell grep -l 'require-env OPENVINO_AVAILABLE' test/sql/*.test))
NER_CHECK_OPENVINO := $(PROJ_DIR)scripts/duckdb-dev.sh -noheader -list -c "SELECT onnx_available FROM anofox_ner_status()" 2>/dev/null | grep -qx true \
		|| { echo "the extension was built without OpenVINO"; exit 1; }
test_ner: release
	@$(NER_CHECK_OPENVINO)
	@for t in $(NER_TESTS); do \
		DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 OPENVINO_AVAILABLE=1 ./build/release/test/unittest "$$t" || exit 1; \
	done

test_ner_multi: release
	@$(NER_CHECK_OPENVINO)
	@for t in $(NER_MULTI_TESTS); do \
		DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 OPENVINO_AVAILABLE=1 ANOFOX_TEST_NER_MULTI=1 ./build/release/test/unittest "$$t" || exit 1; \
	done

.PHONY: test_ner test_ner_multi
