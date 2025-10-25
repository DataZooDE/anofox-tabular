#!/bin/bash
# Install libphonenumber and libpostal from system packages
# This script is used in CI environments via the configure_ci Makefile target

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

    # Check if packages are available in current repos first
    if ! apk search libphonenumber-dev | grep -q libphonenumber-dev; then
        # Enable edge/community repository for libphonenumber-dev and libpostal-dev
        echo "Enabling edge/community repository..."
        echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories
        apk update
    fi

    # Install libpostal-dev (libphonenumber comes from vcpkg)
    echo "Installing libpostal-dev..."
    apk add --no-cache libpostal-dev

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

    # Install build dependencies for libpostal (libphonenumber comes from vcpkg)
    echo "Installing build dependencies for libpostal..."
    $APT_CMD install -y curl autoconf automake libtool pkg-config git

    # Build and install libpostal from source
    echo "Building libpostal from source..."
    LIBPOSTAL_DIR="/tmp/libpostal-build"
    rm -rf "$LIBPOSTAL_DIR"
    git clone https://github.com/openvenues/libpostal "$LIBPOSTAL_DIR"
    cd "$LIBPOSTAL_DIR"

    ./bootstrap.sh
    # Install to /usr/local with data in /usr/local/share/libpostal
    ./configure --datadir=/usr/local/share/libpostal
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

    # Install build dependencies for libpostal (libphonenumber comes from vcpkg)
    echo "Installing build dependencies for libpostal..."
    $YUM_CMD install -y curl autoconf automake libtool pkgconfig git

    # Build and install libpostal from source
    echo "Building libpostal from source..."
    LIBPOSTAL_DIR="/tmp/libpostal-build"
    rm -rf "$LIBPOSTAL_DIR"
    git clone https://github.com/openvenues/libpostal "$LIBPOSTAL_DIR"
    cd "$LIBPOSTAL_DIR"

    ./bootstrap.sh
    # Install to /usr/local with data in /usr/local/share/libpostal
    ./configure --datadir=/usr/local/share/libpostal
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

# Check if library files exist
echo ""
echo "=== Checking for library files ==="
for lib in /usr/lib/libpostal.* /usr/lib/libphonenumber.* /usr/local/lib/libpostal.* /usr/local/lib/libphonenumber.*; do
    if [ -e "$lib" ]; then
        echo "Found: $lib"
    fi
done

# Check for header files
echo ""
echo "=== Checking for header files ==="
if [ -d "/usr/include/libpostal" ]; then
    echo "Found: /usr/include/libpostal"
    ls -la /usr/include/libpostal/ | head -5
fi
if [ -d "/usr/include/phonenumbers" ]; then
    echo "Found: /usr/include/phonenumbers"
    ls -la /usr/include/phonenumbers/ | head -5
fi

# Check for pkg-config files
echo ""
echo "=== Checking for .pc files ==="
for pcfile in /usr/lib/pkgconfig/libpostal.pc /usr/lib/pkgconfig/libphonenumber.pc /usr/local/lib/pkgconfig/libpostal.pc /usr/local/lib/pkgconfig/libphonenumber.pc; do
    if [ -e "$pcfile" ]; then
        echo "Found: $pcfile"
    fi
done

# List all .pc files that might be related
echo ""
echo "=== All pkgconfig files containing 'postal' or 'phone' ==="
find /usr -name "*.pc" 2>/dev/null | grep -E "(postal|phone)" || echo "None found"

echo ""
echo "=== Setup Complete ==="
echo "System packages installed successfully!"
