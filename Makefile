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
# DNNL_MAX_CPU_ISA=AVX2: limits OneDNN (used by OpenVINO) to AVX2 at most,
# preventing SIGILL in manylinux Docker containers that restrict AVX-512 CPUID
test_release_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 ./build/release/test/unittest "test/*"

test_debug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 ./build/debug/test/unittest "test/*"

test_reldebug_internal:
	DATAZOO_DISABLE_TELEMETRY=1 DNNL_MAX_CPU_ISA=AVX2 ./build/reldebug/test/unittest "test/*"
