#!/bin/bash

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
if [ -f "results_$REVIEWER_ID.log" ]; then
    rm "results_$REVIEWER_ID.log"
fi

NUM_WORKERS=1
NUM_EVENTS=10000000
NUM_TXS=1
NUM_FLOWS=1

sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_rate_limit" -t 1 -f $NUM_WORKERS -s 1 -n 10000000 -E 4 -D 32 -C 32
sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_rate_limit" -t 1 -f $NUM_WORKERS -s 1 -n 10000000 -E 4 -D 32 -C 32
echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"

# Redirect all output to one log file
exec > "$REPO_PATH/scripts/fig11e/results_$REVIEWER_ID.log" 2>&1


for NUM_WORKERS in 1 2 3 4 5 6 7 8; do
    echo "Running LDB PORTS with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS"
    sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_ldb_port_rate_limit" -t 1 -f $NUM_WORKERS -s 1 -n 100000000 -E 4 -D 32 -C 32
    echo "========================================"
done

for NUM_WORKERS in 1 2 3 4 5 6 7 8; do
    echo "Running DIR PORTS with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS"
    sudo taskset -c 0-14 "$REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_rate_limit" -t 1 -f $NUM_WORKERS -s 1 -n 100000000 -E 4 -D 32 -C 32
    echo "========================================"
done