#!/bin/bash
set -e

host="$1"

# Validate the input
if [[ "$host" != "server" && "$host" != "snic" && "$host" != "client" ]]; then
    echo "Invalid host specified. Valid options are: server, snic, client."
    exit 1
fi

source env_setup.sh

###### Build Masstree ######
MASSTREE_DIR="$REPO_PATH/src/masstree"

## baseline
if [[ "$host" != "snic" ]]; then
    cd "$MASSTREE_DIR/baseline"
    echo "==== baseline ===="
    make clean
    make -j
fi

## dlb
cd "$MASSTREE_DIR/dlb"
echo "==== dlb ===="
case "$host" in
    server)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 1/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 0/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        make clean
        make -j
        ;;
    snic)
        cd "$MASSTREE_DIR/dlb/rdma-dlb"
        make clean
        make -j
        ;;
    client)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 0/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 1/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$MASSTREE_DIR/dlb/RDMAConnectionRC.h"
        make clean
        make -j
        ;;
esac


## dpdk-pd
if [[ "$host" != "snic" ]]; then
    cd "$MASSTREE_DIR/dpdk-pd"
    echo "==== dpdk-pd ===="
    make clean
    make -j
fi
