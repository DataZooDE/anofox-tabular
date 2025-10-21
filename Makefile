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

# Override configure_ci to install missing dependencies in Docker
# This is called inside the Docker container before the build starts
configure_ci:
	@echo "Installing missing dependencies for ICU build..."
	@if command -v yum > /dev/null 2>&1; then \
		echo "Installing autoconf-archive via yum..."; \
		yum install -y autoconf-archive || true; \
	elif command -v apt-get > /dev/null 2>&1; then \
		echo "Installing autoconf-archive via apt..."; \
		apt-get update && apt-get install -y autoconf-archive || true; \
	else \
		echo "Warning: Neither yum nor apt-get found, skipping dependency installation"; \
	fi
	@echo "configure_ci completed successfully"