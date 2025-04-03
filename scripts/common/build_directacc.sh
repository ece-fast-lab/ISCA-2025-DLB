#!/bin/bash
set -e

host="$1"

# Validate the input
if [[ "$host" != "server" && "$host" != "snic" && "$host" != "client" ]]; then
    echo "Invalid host specified. Valid options are: server, snic, client."
    exit 1
fi

source env_setup.sh


###### Build DirectAcc ######
DIRECT_ACC_DIR="$REPO_PATH/src/directacc"

## offload directacc
cd "$DIRECT_ACC_DIR/offload-rdma-dlb"
case "$host" in
    server)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 1/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        make clean
        make rdma_dlb_server
        ;;
    snic)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 1/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        make clean
        make rdma_dlb_snic
        ;;
    client)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 1/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$DIRECT_ACC_DIR/offload-rdma-dlb/common.h"
        make clean
        make rdma_dlb_client
        ;;
esac

## cpu
cd "$DIRECT_ACC_DIR/cpu-rdma-dlb"
case "$host" in
    server)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 1/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        make clean
        make rdma_dlb_server
        ;;
    snic)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 1/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        make clean
        make rdma_dlb_snic
        ;;
    client)
        sed -i 's/^#define IS_SERVER .*/#define IS_SERVER 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_CLIENT .*/#define IS_CLIENT 1/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        sed -i 's/^#define IS_SNIC .*/#define IS_SNIC 0/' "$DIRECT_ACC_DIR/cpu-rdma-dlb/common.h"
        make clean
        make rdma_dlb_client
        ;;
esac