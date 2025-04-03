#!/usr/bin/env bash

source ../common/env_setup.sh

DLB_DIR=$REPO_PATH/src/dlb_8.9.0
DPDK_DIR=$REPO_PATH/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2

dlb=$(lspci -d :2710)
echo "Get DLB device $dlb"

pat='[0-9a-fA-F]+\:[0-9a-fA-F]+\.[0-9a-fA-F]+'
[[ $dlb =~ $pat ]]
pci_addr=${BASH_REMATCH[0]}
echo "DLB PCIe ID to be unbind: $pci_addr"

echo "unbinding..."
$DPDK_DIR/usertools/dpdk-devbind.py --unbind $pci_addr

rmmod dlb2_pmem
rmmod dlb2