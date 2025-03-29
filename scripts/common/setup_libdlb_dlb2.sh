#!/usr/bin/env bash

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

DLB_DIR=$REPO_PATH/src/dlb_8.9.0

cp $DLB_DIR/libdlb/libdlb.so /usr/lib/.

insmod $DLB_DIR/driver/dlb2/dlb2.ko
insmod $DLB_DIR/driver/dlb2/pmem/dlb2_pmem.ko

