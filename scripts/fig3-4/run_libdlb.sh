#!/bin/bash

set -e

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

prepare_logs() {
    log_dir="$REPO_PATH/scripts/fig3-4/results_$REVIEWER_ID"
    if [ ! -d "$log_dir" ]; then
        echo "Creating directory $log_dir"
        mkdir "$log_dir"
    else
        echo "Directory $log_dir already exists."
    fi
}

prepare_logs

echo "Running dlb-lib for figure 3"
for NUM_WORKERS in 1 2 3 4 5 6 7 8; do
    log_file="$log_dir/dlb-lib_TX8_RX${NUM_WORKERS}.txt"
    sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_rate_limit" -t 8 -f $NUM_WORKERS -s 1 -n 100000000 -E 32 -D 32 -C 32 > $log_file
    echo "========================================"
done

echo "Running dlb-lib for figure 4"
for NUM_PROD in 1 2 3 4 5 6 7 8; do
    log_file="$log_dir/dlb-lib_TX${NUM_PROD}_RX8.txt"
    sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_rate_limit" -t $NUM_PROD -f 8 -s 1 -n 100000000 -E 32 -D 32 -C 32 > $log_file
    echo "========================================"
done
