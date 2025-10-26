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

# Force all builds to use "build" directory (remove -ninja suffix)
# Override BUILD_ROOT after include to force unified build directory
BUILD_ROOT:=build
DEBUG_BUILD_DIR:=$(BUILD_ROOT)/debug
RELEASE_BUILD_DIR:=$(BUILD_ROOT)/release

# Override configure_ci to run dependency installation script
# The script will install/build libpostal based on the environment
configure_ci:
	bash $(PROJ_DIR)/scripts/install-deps-ci.sh
