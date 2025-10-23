#!/bin/bash
# Install libphonenumber and libpostal as system packages or build from source
# This script is used in CI environments via the configure_ci Makefile target

set -e

# Configuration
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/anofox-deps}"
BUILD_DIR="${BUILD_DIR:-/tmp/anofox-build}"

echo "=== Installing CI dependencies for anofox_tabular ==="
echo "Install prefix: $INSTALL_PREFIX"
echo "Build directory: $BUILD_DIR"

# Detect package manager
if command -v yum &> /dev/null; then
    PKG_MANAGER="yum"
    INSTALL_CMD="yum install -y"
elif command -v apt-get &> /dev/null; then
    PKG_MANAGER="apt"
    INSTALL_CMD="apt-get install -y"
    # Update package lists for apt
    apt-get update
else
    echo "ERROR: No supported package manager found (yum or apt-get)"
    exit 1
fi

echo "Using package manager: $PKG_MANAGER"

# Install build dependencies
echo "Installing build dependencies..."
if [ "$PKG_MANAGER" = "yum" ]; then
    yum install -y \
        cmake \
        git \
        make \
        gcc-c++ \
        protobuf-devel \
        protobuf-compiler \
        libicu-devel \
        boost-devel \
        boost-system \
        boost-thread \
        re2-devel \
        openssl-devel \
        curl \
        pkg-config
elif [ "$PKG_MANAGER" = "apt" ]; then
    apt-get install -y \
        cmake \
        git \
        make \
        g++ \
        protobuf-compiler \
        libprotobuf-dev \
        libicu-dev \
        libboost-dev \
        libboost-system-dev \
        libboost-thread-dev \
        libre2-dev \
        libssl-dev \
        curl \
        pkg-config
fi

# Create build directory
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_PREFIX"/{lib,include,lib/pkgconfig}

echo ""
echo "=== Building libphonenumber ==="

# Download and build libphonenumber
LIBPHONENUMBER_VERSION="8.13.39"
LIBPHONENUMBER_URL="https://github.com/google/libphonenumber/archive/v${LIBPHONENUMBER_VERSION}.tar.gz"
LIBPHONENUMBER_SRC="$BUILD_DIR/libphonenumber-${LIBPHONENUMBER_VERSION}"

if [ ! -d "$LIBPHONENUMBER_SRC" ]; then
    echo "Downloading libphonenumber v${LIBPHONENUMBER_VERSION}..."
    cd "$BUILD_DIR"
    curl -L "$LIBPHONENUMBER_URL" | tar xz
fi

# Build libphonenumber
cd "$LIBPHONENUMBER_SRC/cpp"
mkdir -p build
cd build

echo "Configuring libphonenumber..."
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
    -DUSE_BOOST=ON \
    ..

echo "Building libphonenumber..."
make -j$(nproc)

echo "Installing libphonenumber..."
make install

echo ""
echo "=== Checking for libpostal ==="

# Check if libpostal is available as a system package
if pkg-config --exists libpostal; then
    echo "libpostal found via pkg-config (system package)"
    LIBPOSTAL_PC=$(pkg-config --variable=pcfiledir libpostal)/libpostal.pc
    echo "  Location: $LIBPOSTAL_PC"
else
    echo "WARNING: libpostal not found via pkg-config"
    echo "  Attempting to install libpostal development package..."

    if [ "$PKG_MANAGER" = "yum" ]; then
        yum install -y libpostal-devel || echo "WARNING: Could not install libpostal-devel via yum"
    elif [ "$PKG_MANAGER" = "apt" ]; then
        apt-get install -y libpostal-dev || echo "WARNING: Could not install libpostal-dev via apt"
    fi
fi

# Export environment variables for CMake
export PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$INSTALL_PREFIX/lib:${LD_LIBRARY_PATH}"

# Verify pkg-config can find both libraries
echo ""
echo "=== Verifying pkg-config setup ==="
if pkg-config --exists libphonenumber; then
    echo "✓ libphonenumber found"
    pkg-config --modversion libphonenumber
else
    echo "✗ ERROR: libphonenumber not found in pkg-config"
    exit 1
fi

if pkg-config --exists libpostal; then
    echo "✓ libpostal found"
    pkg-config --modversion libpostal
else
    echo "✗ WARNING: libpostal not found in pkg-config"
fi

# Create environment setup file for subsequent build steps
SETUP_FILE="${INSTALL_PREFIX}/setup-env.sh"
mkdir -p "$(dirname "$SETUP_FILE")"
cat > "$SETUP_FILE" << 'EOF'
#!/bin/bash
# Generated environment setup for anofox_tabular CI build
export PKG_CONFIG_PATH="/opt/anofox-deps/lib/pkgconfig:${PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="/opt/anofox-deps/lib:${LD_LIBRARY_PATH}"
EOF
chmod +x "$SETUP_FILE"

echo ""
echo "=== Setup Complete ==="
echo "Environment setup file created: $SETUP_FILE"
echo ""
echo "For subsequent builds, source this file:"
echo "  source $SETUP_FILE"
echo ""
echo "Or add to your environment for local builds:"
echo "export PKG_CONFIG_PATH='$INSTALL_PREFIX/lib/pkgconfig:\$PKG_CONFIG_PATH'"
echo "export LD_LIBRARY_PATH='$INSTALL_PREFIX/lib:\$LD_LIBRARY_PATH'"
