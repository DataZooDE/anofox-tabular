#!/bin/bash
# Install libpostal from source for anofox_tabular extension
# This script is used in CI environments via the configure_ci Makefile target
#
# Dependencies:
# - libphonenumber: Internal implementation (no external dependency required)
# - libpostal: Built from source with CFLAGS=-fPIC for shared library linking

set -e

echo "=== Installing CI dependencies for anofox_tabular ==="

# Skip if running outside Docker - dependencies will be installed inside Docker container
# This applies to all Linux builds (both glibc and musl)
if [ "$LINUX_CI_IN_DOCKER" = "0" ]; then
    echo "Skipping package installation on host (LINUX_CI_IN_DOCKER=0)"
    echo "Dependencies will be installed inside Docker container"
    exit 0
fi

# Detect package manager
if command -v apk &> /dev/null; then
    PKG_MANAGER="apk"
    echo "Using Alpine Linux (apk)"

    # Update package list
    apk update

    # Install build dependencies
    echo "Installing build dependencies..."
    apk add --no-cache curl autoconf automake libtool pkgconf git

    # Build and install libpostal from source with -fPIC
    echo "Building libpostal from source with -fPIC..."
    LIBPOSTAL_DIR="/tmp/libpostal-build"
    rm -rf "$LIBPOSTAL_DIR"
    git clone https://github.com/openvenues/libpostal "$LIBPOSTAL_DIR"
    cd "$LIBPOSTAL_DIR"

    ./bootstrap.sh
    # Install to /usr/local with data in /usr/local/share/libpostal
    # CRITICAL: Use CFLAGS=-fPIC to enable linking into shared libraries
    CFLAGS="-fPIC" ./configure --datadir=/usr/local/share/libpostal
    make -j$(nproc)
    make install

    # Update library cache (if ldconfig is available)
    # In Alpine/musl, ldconfig is a no-op that may return non-zero, so ignore errors
    if command -v ldconfig &> /dev/null; then
        ldconfig || true
    fi

    # Clean up
    cd /
    rm -rf "$LIBPOSTAL_DIR"

    echo "libpostal built and installed successfully with -fPIC"

elif command -v apt-get &> /dev/null; then
    PKG_MANAGER="apt"
    echo "Using Debian/Ubuntu (apt)"

    # Use sudo if not root
    if [ "$EUID" -ne 0 ]; then
        APT_CMD="sudo apt-get"
        SUDO_CMD="sudo"
    else
        APT_CMD="apt-get"
        SUDO_CMD=""
    fi

    # Update package lists
    $APT_CMD update

    # Install build dependencies
    echo "Installing build dependencies..."
    $APT_CMD install -y curl autoconf automake libtool pkg-config git

    # Build and install libpostal from source
    echo "Building libpostal from source..."
    LIBPOSTAL_DIR="/tmp/libpostal-build"
    rm -rf "$LIBPOSTAL_DIR"
    git clone https://github.com/openvenues/libpostal "$LIBPOSTAL_DIR"
    cd "$LIBPOSTAL_DIR"

    ./bootstrap.sh
    # Install to /usr/local with data in /usr/local/share/libpostal
    # CRITICAL: Use CFLAGS=-fPIC to enable linking into shared libraries
    CFLAGS="-fPIC" ./configure --datadir=/usr/local/share/libpostal
    make -j$(nproc)
    $SUDO_CMD make install

    # Update library cache
    $SUDO_CMD ldconfig

    # Clean up
    cd /
    rm -rf "$LIBPOSTAL_DIR"

    echo "libpostal built and installed successfully"

elif command -v yum &> /dev/null; then
    PKG_MANAGER="yum"
    echo "Using RHEL/CentOS (yum)"

    # Use sudo if not root
    if [ "$EUID" -ne 0 ]; then
        YUM_CMD="sudo yum"
        SUDO_CMD="sudo"
    else
        YUM_CMD="yum"
        SUDO_CMD=""
    fi

    # Install build dependencies
    echo "Installing build dependencies..."
    $YUM_CMD install -y curl autoconf automake libtool pkgconfig git

    # Build and install libpostal from source
    echo "Building libpostal from source..."
    LIBPOSTAL_DIR="/tmp/libpostal-build"
    rm -rf "$LIBPOSTAL_DIR"
    git clone https://github.com/openvenues/libpostal "$LIBPOSTAL_DIR"
    cd "$LIBPOSTAL_DIR"

    ./bootstrap.sh
    # Install to /usr/local with data in /usr/local/share/libpostal
    # CRITICAL: Use CFLAGS=-fPIC to enable linking into shared libraries
    CFLAGS="-fPIC" ./configure --datadir=/usr/local/share/libpostal
    make -j$(nproc)
    $SUDO_CMD make install

    # Update library cache
    $SUDO_CMD ldconfig

    # Clean up
    cd /
    rm -rf "$LIBPOSTAL_DIR"

    echo "libpostal built and installed successfully"

else
    echo "ERROR: No supported package manager found (apk, apt-get, or yum)"
    exit 1
fi

# Verify installation
echo ""
echo "=== Verifying installation ==="

if [ -f "/usr/local/lib/libpostal.a" ] && [ -f "/usr/local/include/libpostal/libpostal.h" ]; then
    echo "✓ libpostal installed successfully in /usr/local"
else
    echo "⚠ Warning: libpostal files not found in expected location"
    echo "  Expected: /usr/local/lib/libpostal.a and /usr/local/include/libpostal/libpostal.h"
fi

echo ""
echo "=== Setup Complete ==="
