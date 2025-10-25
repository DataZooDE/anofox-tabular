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
# For musl: installs in Alpine Docker container via apk
# For glibc: installs on Ubuntu/Debian host via apt
configure_ci:
	bash $(PROJ_DIR)/scripts/install-deps-ci.sh

# Debug target to output vcpkg build logs on failure
.PHONY: debug_vcpkg_logs
debug_vcpkg_logs:
	@echo "=== Checking for vcpkg build error logs ==="
	@if [ -f "build/release/vcpkg_installed/vcpkg/buildtrees/icu/autoconf-x64-linux-err.log" ]; then \
		echo "=== ICU autoconf error log ==="; \
		cat build/release/vcpkg_installed/vcpkg/buildtrees/icu/autoconf-x64-linux-err.log; \
	fi
	@if [ -f "build/release/vcpkg-manifest-install.log" ]; then \
		echo "=== vcpkg manifest install log ==="; \
		tail -100 build/release/vcpkg-manifest-install.log; \
	fi