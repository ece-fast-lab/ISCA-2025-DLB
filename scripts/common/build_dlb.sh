#!/bin/bash
set -e

source env_setup.sh

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
