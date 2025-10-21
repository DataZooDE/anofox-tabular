# Default ARM64 Linux triplet (used for host builds)
# Ensures release-only builds for all host tools
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Only build release configuration (skip debug) - CRITICAL for host builds
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
