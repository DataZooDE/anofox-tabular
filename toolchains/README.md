# Custom vcpkg Triplets

This directory contains custom vcpkg triplet files for the Anofox Tabular extension build.

## Purpose

These triplets override the default vcpkg community triplets to fix build issues, specifically:

- **Skip debug builds**: Sets `VCPKG_BUILD_TYPE release` to only build release configurations
- **Avoid ICU build failures**: The ICU library (dependency of libphonenumber) fails to build in debug mode in the CI environment

## Files

- `x64-linux-release.cmake` - Custom triplet for x64 Linux builds
- `arm64-linux-release.cmake` - Custom triplet for ARM64 Linux builds

## Configuration

These triplets are enabled via the `overlay-triplets` setting in `vcpkg.json`:

```json
"overlay-triplets": [
    "./toolchains",
    "./extension-ci-tools/toolchains"
]
```

The local `./toolchains` directory is searched first, allowing us to override the default community triplets from vcpkg.

## Changes from Community Triplets

The main difference from the vcpkg community `x64-linux-release` triplet is:

- Explicitly sets `VCPKG_BUILD_TYPE release` to skip debug builds entirely
- Uses static linking for libraries
- Sets dynamic CRT linkage

This matches the pattern used in the Windows triplet (`x64-windows-static-md-release-vs2019comp.cmake`) in the extension-ci-tools.
