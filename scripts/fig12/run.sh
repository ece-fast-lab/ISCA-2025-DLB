#!/bin/bash

set -e

set_default() {
    source ../common/env_setup.sh
    NUM_EVENTS=100000000
    NUM_WORKERS=1
    NUM_TXS=1
    NUM_FLOWS=1
    ENQUEUE_DEPTH=32
    DEQUEUE_DEPTH=32
    CQ_DEPTH=32
    log_dir="$REPO_PATH/scripts/fig12/results_$REVIEWER_ID"
}

prepare_logs() {
    log_dir="$REPO_PATH/scripts/fig12/results_$REVIEWER_ID"
    if [ ! -d "$log_dir" ]; then
        echo "Creating directory $log_dir"
        mkdir "$log_dir"
    else
        echo "Directory $log_dir already exists."
    fi
}

packet_prio() {
    set_default
    prepare_logs
    NUM_WORKERS=8
    echo "Running packet priority"
    for NUM_TXS in 8; do    
        for i in 0 1 2 3 4 5 6 7; do
            local log_file="$log_dir/packet_prio_priority_tx${NUM_TXS}_${i}.txt"
            # echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH, PRIORITY=$i"
            sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_qos -t $NUM_TXS -f $NUM_WORKERS -s 1 -n $NUM_EVENTS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH -P 1 > $log_file
        done
    done
}

queue_prio() {
    set_default
    prepare_logs
    NUM_TXS=8
    # NUM_WORKERS=8
    echo "Running queue priority"
    for NUM_WORKERS in 8; do    
        for i in 0 1 2 3 4 5 6 7; do
            local log_file="$log_dir/queue_prio_priority_worker${NUM_WORKERS}_${i}.txt"
            # echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH, PRIORITY=$i"
            sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_qos -t $NUM_TXS -f $NUM_WORKERS -s 1 -n $NUM_EVENTS -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH -Q 1 > $log_file
        done
    done
}

mix_prio() {
    set_default
    prepare_logs
    NUM_TXS=8
    NUM_WORKERS=1
    ENQUEUE_DEPTH=4
    DEQUEUE_DEPTH=8
    CQ_DEPTH=8
    echo "Running mix priority"
    for i in 0 1 2 3 4 5 6 7; do
        local log_file="$log_dir/mix_prio_priority_tx${NUM_TXS}_worker${NUM_WORKERS}_${i}.txt"
        # echo "Running with NUM_EVENTS = $NUM_EVENTS, NUM_TXS=$NUM_TXS, NUM_WORKERS=$NUM_WORKERS, NUM_FLOWS=$NUM_FLOWS, ENQUEUE_DEPTH=$ENQUEUE_DEPTH, CQ_DEPTH=$CQ_DEPTH, PRIORITY=$i"
        sudo taskset -c 0-14 $REPO_PATH/src/dlb_bench/libdlb_bench/ldb_traffic_qos -w interrupt -t $NUM_TXS -f $NUM_WORKERS -s 1 -n 8000000 -E $ENQUEUE_DEPTH -D $CQ_DEPTH -C $CQ_DEPTH -P 1 -Q 1 > $log_file
    done
}


if [ $# -eq 0 ]; then
    echo "No arguments provided. Running packet_prio, queue_prio, and mix_prio in sequence."
    packet_prio
    queue_prio
    mix_prio
else
    "$@"
fi


"$@"
