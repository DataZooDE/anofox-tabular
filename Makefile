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

# Override configure_ci to build libpostal from source
# This ensures libpostal is built in the same Docker container that will run the build
configure_ci:
	@echo "=== Building libpostal from source ==="
	@if [ ! -d "libpostal-src" ]; then \
		echo "Cloning libpostal..."; \
		git clone --depth 1 https://github.com/openvenues/libpostal libpostal-src; \
	fi
	cd libpostal-src && \
	./bootstrap.sh && \
	./configure --datadir=/usr/share/libpostal --disable-sse2 && \
	make -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) && \
	make install
	@echo "=== libpostal installation complete ==="

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