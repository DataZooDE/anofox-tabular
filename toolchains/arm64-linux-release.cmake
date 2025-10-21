# Custom triplet for ARM64 Linux release-only builds
# This triplet skips debug builds to avoid issues with ICU and other dependencies

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Only build release configuration (skip debug)
set(VCPKG_BUILD_TYPE release)

# Set CMAKE_SYSTEM_NAME to ensure cross-compilation settings
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
