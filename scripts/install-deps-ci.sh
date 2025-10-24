#!/bin/bash
# Install libphonenumber and libpostal from system packages
# This script is used in CI environments via the configure_ci Makefile target

set -e

echo "=== Installing CI dependencies for anofox_tabular ==="

# For musl builds: skip if running outside Docker (packages will be installed inside)
if [ "$LINUX_CI_IN_DOCKER" = "0" ] && [ "$DUCKDB_PLATFORM" = "linux_amd64_musl" ]; then
    echo "Skipping package installation on host for musl build (will install inside Docker)"
    exit 0
fi

if [ "$LINUX_CI_IN_DOCKER" = "0" ] && [ "$DUCKDB_PLATFORM" = "linux_arm64_musl" ]; then
    echo "Skipping package installation on host for musl build (will install inside Docker)"
    exit 0
fi

# Detect package manager
if command -v apk &> /dev/null; then
    PKG_MANAGER="apk"
    echo "Using Alpine Linux (apk)"

    # Update package list
    apk update

    # Check if packages are available in current repos first
    if ! apk search libphonenumber-dev | grep -q libphonenumber-dev; then
        # Enable edge/community repository for libphonenumber-dev and libpostal-dev
        echo "Enabling edge/community repository..."
        echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories
        apk update
    fi

    # Install libphonenumber-dev and libpostal-dev
    echo "Installing libphonenumber-dev and libpostal-dev..."
    apk add --no-cache \
        libphonenumber-dev \
        libpostal-dev

elif command -v apt-get &> /dev/null; then
    PKG_MANAGER="apt"
    echo "Using Debian/Ubuntu (apt)"

    # Use sudo if not root
    if [ "$EUID" -ne 0 ]; then
        APT_CMD="sudo apt-get"
    else
        APT_CMD="apt-get"
    fi

    # Update package lists
    $APT_CMD update

    # Install libphonenumber and libpostal development packages
    echo "Installing libphonenumber-dev and libpostal-dev..."
    $APT_CMD install -y \
        libphonenumber-dev \
        libpostal-dev

elif command -v yum &> /dev/null; then
    PKG_MANAGER="yum"
    echo "Using RHEL/CentOS (yum)"

    # Use sudo if not root
    if [ "$EUID" -ne 0 ]; then
        YUM_CMD="sudo yum"
    else
        YUM_CMD="yum"
    fi

    # Install libphonenumber and libpostal development packages
    echo "Installing libphonenumber-devel and libpostal-devel..."
    $YUM_CMD install -y \
        libphonenumber-devel \
        libpostal-devel

else
    echo "ERROR: No supported package manager found (apk, apt-get, or yum)"
    exit 1
fi

# Verify installation with pkg-config
echo ""
echo "=== Verifying installation ==="

if pkg-config --exists libphonenumber 2>/dev/null; then
    echo "✓ libphonenumber found via pkg-config"
    pkg-config --modversion libphonenumber
else
    echo "⚠ libphonenumber not found via pkg-config, but may be installed"
fi

if pkg-config --exists libpostal 2>/dev/null; then
    echo "✓ libpostal found via pkg-config"
    pkg-config --modversion libpostal
else
    echo "⚠ libpostal not found via pkg-config, but may be installed"
fi

echo ""
echo "=== Setup Complete ==="
echo "System packages installed successfully!"
