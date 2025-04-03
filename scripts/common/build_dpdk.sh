#!/bin/bash
set -e

source env_setup.sh

###### Build DPDK ######
DPDK_DIR="$REPO_PATH/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2"
DPDK_BUILD_DIR="build"

cd "$DPDK_DIR"

if [ -d "$DPDK_BUILD_DIR" ]; then
    echo "Removing previous build directory: $DPDK_BUILD_DIR"
    rm -rf "$DPDK_BUILD_DIR"
fi

# Configure the build using Meson.
echo "Configuring DPDK build with Meson..."
meson setup -Dexamples=all "$DPDK_BUILD_DIR"

# Build DPDK using Ninja.
echo "Building DPDK with Ninja..."
ninja -C "$DPDK_BUILD_DIR"

echo "DPDK build completed."