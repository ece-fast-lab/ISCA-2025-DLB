#!/bin/bash

set -e

source ../common/env_setup.sh
if [ -f "results_$REVIEWER_ID.log" ]; then
    rm "results_$REVIEWER_ID.log"
fi

sudo ../common/setup_libdlb_dlb2.sh

exec > "$REPO_PATH/scripts/fig13/results_$REVIEWER_ID.log" 2>&1


set_default() {
    NUM_WORKERS=1
    NUM_EVENTS=10000000
    NUM_TXS=1
    NUM_FLOWS=1
    BATCH_SIZE=4
    ENQUEUE_DEPTH=4
    DEQUEUE_DEPTH=4
    CQ_DEPTH=8
    log_dir="$REPO_PATH/scripts/fig13/results_$REVIEWER_ID"
}

prepare_logs() {
    log_dir="$REPO_PATH/scripts/fig13/results_$REVIEWER_ID"
    if [ ! -d "$log_dir" ]; then
        echo "Creating directory $log_dir"
        mkdir "$log_dir"
    else
        echo "Directory $log_dir already exists."
    fi
}

cq_depth() {
    ENQUEUE_DEPTH=128
    NUM_FLOWS=64
    for CQ_DEPTH in 4 8 16 32 64 128; do
        echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -t 8 -f 8 -s 0 -n 10000000 -F $NUM_FLOWS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH
    done
}

enqueue_depth() {
    for ENQUEUE_DEPTH in 4 8 16 32 64 128 256 512 1024; do
        echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -t 1 -f 8 -s 0 -n 10000000 -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH
    done
}

multi_workers() {
    set_default
    NUM_TXS=${1:-1}
    NUM_WORKERS=${2:-8}
    NUM_FLOWS=${3:-128}
    ENQUEUE_DEPTH=8
    CQ_DEPTH=32
    for NUM_WORKERS in 1 2 3 4 5 6 7 8; do
        # NUM_EVENTS=$((10000000*$NUM_WORKERS))
        echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -t 1 -f $NUM_WORKERS -s 0 -n 10000000 -F $NUM_FLOWS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH
        echo "========================================"
    done
}

multi_txs() {
    set_default
    NUM_TXS=${1:-1}
    NUM_WORKERS=${2:-8}
    NUM_FLOWS=${3:-128}
    ENQUEUE_DEPTH=8
    CQ_DEPTH=32
    for NUM_TXS in 1 2 3 4 5 6 7 8; do
        echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -t $NUM_TXS -f 1 -s 0 -n 10000000 -F $NUM_FLOWS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH
        echo "========================================"
    done
}

multi_flows() {
    set_default
    NUM_TXS=${1:-8}
    NUM_WORKERS=${2:-8}
    NUM_FLOWS=${3:-128}
    ENQUEUE_DEPTH=8
    CQ_DEPTH=32
    for NUM_FLOWS in 8 16 32 64 128 256 512 1024 2048; do
        echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -w interrupt -t $NUM_TXS -f $NUM_WORKERS -s 0 -n $((100000*$NUM_FLOWS)) -F $NUM_FLOWS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH
        echo "========================================"
    done
}

atomic_test() {
    set_default
    NUM_TXS=${1:-1}
    NUM_WORKERS=${2:-8}
    NUM_FLOWS=${3:-1024}
    ENQUEUE_DEPTH=4
    CQ_DEPTH=8
    for CQ_DEPTH in 8 16 32 64 128; do
        for ENQUEUE_DEPTH in 4 8 16 32 64; do
            echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH"
            sudo $REPO_PATH/src/dlb_bench/libdlb_bench/test_atomic_queue -t $NUM_TXS -f $NUM_WORKERS -s 0 -n 10000000 -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH -F $NUM_FLOWS
            sleep 1
        done
    done
}


if [ $# -eq 0 ]; then
    echo "No arguments provided. Running multi_flows for figure 13."
    multi_flows
else
    "$@"
fi


"$@"
