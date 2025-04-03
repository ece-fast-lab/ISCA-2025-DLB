#!/bin/bash

# set -e

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
DPDK_APP_DIR=$REPO_PATH/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2/build/app
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

echo "Running dpdk-pd for figure 3"
for NUM_WORKER in 1 2 3 4 5 6 7 8; do
    log_file="$log_dir/dpdk-pd_TX8_RX${NUM_WORKER}.txt"
    sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-distributor-c2c -l 0-$((10+$NUM_WORKER)) -a "17:00.0" -- -p 1 > $log_file
    echo "========================================"
done


echo "Running dpdk-ed for figure 3"
log_file="$log_dir/dpdk-ed_TX8_RX1.txt"
sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" --vdev="event_sw0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2-9 --nb_pkts 0 --wlcores=16 > $log_file
echo "========================================"
for NUM_WORKER in 17 18 19 20 21 22 23; do
    log_file="$log_dir/dpdk-ed_TX8_RX$(($NUM_WORKER-15)).txt"
    sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" --vdev="event_sw0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2-9 --nb_pkts 0 --wlcores=16-$NUM_WORKER > $log_file
    echo "========================================"
done


echo "Running dlb-ed for figure 3"
log_file="$log_dir/dlb-ed_TX8_RX1.txt"
sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" -a "0000:f5:00.0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2-9 --nb_pkts 0 --wlcores=16 > $log_file
echo "========================================"
for NUM_WORKER in 17 18 19 20 21 22 23; do
    log_file="$log_dir/dlb-ed_TX8_RX$(($NUM_WORKER-15)).txt"
    sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" -a "0000:f5:00.0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2-9 --nb_pkts 0 --wlcores=16-$NUM_WORKER > $log_file
    echo "========================================"
done


echo "Running dlb-ed for figure 4"
log_file="$log_dir/dlb-ed_TX1_RX8.txt"
sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" -a "0000:f5:00.0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2 --nb_pkts 0 --wlcores=16-23 > $log_file
echo "========================================"
for NUM_PROD in 3 4 5 6 7 8 9; do
    log_file="$log_dir/dlb-ed_TX$(($NUM_PROD-1))_RX8.txt"
    sudo -S timeout --signal=SIGINT 10s $DPDK_APP_DIR/dpdk-test-eventdev -c 0xffffffff -s 0x3 -a "0000:17:00.0" -a "0000:f5:00.0" -- --test=perf_queue --stlist=p --prod_enq_burst_sz=128 --worker_deq_depth=128 --plcores=2-$NUM_PROD --nb_pkts 0 --wlcores=16-23 > $log_file
    echo "========================================"
done
