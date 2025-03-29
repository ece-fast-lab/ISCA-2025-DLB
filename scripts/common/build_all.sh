#!/bin/bash
set -e

source env_setup.sh

cd "$DPDK_DIR"

###### Build DLB Drivers and libdlb ######
DLB_DIR="$REPO_PATH/src/dlb_8.9.0"
cd "$DLB_DIR/driver/dlb2"
make clean
make

cd "pmem"
make clean
make

cd "$DLB_DIR/libdlb"
make clean
make lib

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


###### Build dpdk Bench ######
export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig/
DPDK_BENCH_DIR="$REPO_PATH/src/dlb_bench/dpdk_bench"
cd "$DPDK_BENCH_DIR/dpdk-rx"
make clean
make

cd "$DPDK_BENCH_DIR/dpdk-tx"
make clean
make


###### Build libdlb Bench ######
DLB_BENCH_DIR="$REPO_PATH/src/dlb_bench/libdlb_bench"
cd "$DLB_BENCH_DIR"
make clean
make


