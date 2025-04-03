#!/bin/bash
set -e

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 {server|snic|client}"
    exit 1
fi

host="$1"

# Validate the input
if [[ "$host" != "server" && "$host" != "snic" && "$host" != "client" ]]; then
    echo "Invalid host specified. Valid options are: server, snic, client."
    exit 1
fi

echo "Compile for host: $host"


source env_setup.sh


###### Build DLB Drivers and libdlb ######
# bash "$REPO_PATH/scripts/common/build_dlb.sh"

###### Build DPDK ######
# bash "$REPO_PATH/scripts/common/build_dpdk.sh"


###### Build dpdk bench, libdlb bench, and DirectAcc ######
case "$host" in
    server)
        bash $REPO_PATH/scripts/common/build_bench.sh server
        ;;
    snic)
        bash $REPO_PATH/scripts/common/build_directacc.sh snic
        ;;
    client)
        bash $REPO_PATH/scripts/common/build_bench.sh client
        ;;
esac


###### Build Masstree ######
case "$host" in
    server)
        bash $REPO_PATH/scripts/common/build_masstree.sh server
        ;;
    snic)
        bash $REPO_PATH/scripts/common/build_masstree.sh snic
        ;;
    client)
        bash $REPO_PATH/scripts/common/build_masstree.sh client
        ;;
esac
