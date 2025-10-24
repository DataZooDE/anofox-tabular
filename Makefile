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

# Override configure_ci to install system packages for libphonenumber and libpostal
# Installs from Alpine edge/community or Ubuntu/Debian packages
configure_ci:
	bash $(PROJ_DIR)/scripts/install-deps-ci.sh