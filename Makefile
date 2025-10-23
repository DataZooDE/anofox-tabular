PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# VCPKG setup: Set the VCPKG_TOOLCHAIN_PATH variable that the included makefile expects.
ifeq ($(VCPKG_ROOT),)
	export VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)/vcpkg_installed/$(VCPKG_TARGET_TRIPLET)/share/vcpkg/scripts/buildsystems/vcpkg.cmake
else
	export VCPKG_TOOLCHAIN_PATH ?= $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
endif

# Setup environment for libphonenumber and libpostal from system packages
# This is set if the install-deps-ci.sh script has run
ifneq ("$(wildcard /opt/anofox-deps/setup-env.sh)","")
	$(shell source /opt/anofox-deps/setup-env.sh)
	export PKG_CONFIG_PATH=/opt/anofox-deps/lib/pkgconfig:$(PKG_CONFIG_PATH)
	export LD_LIBRARY_PATH=/opt/anofox-deps/lib:$(LD_LIBRARY_PATH)
endif

# Configuration of extension
EXT_NAME=anofox_tabular
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override configure_ci to build and install libphonenumber and libpostal
# This is called inside the Docker container before the build starts
configure_ci:
	@echo "Setting up CI dependencies..."
	@bash $(PROJ_DIR)/scripts/install-deps-ci.sh
	@echo "configure_ci completed successfully"