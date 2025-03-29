#!/usr/bin/env bash

source ../common/env_setup.sh

DLB_DIR=$REPO_PATH/src/dlb_8.9.0
DPDK_DIR=$REPO_PATH/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2

insmod $DLB_DIR/driver/dlb2/dlb2.ko
insmod $DLB_DIR/driver/dlb2/pmem/dlb2_pmem.ko
modprobe vfio
modprobe vfio-pci

dlb=$(lspci -d :2710)
echo "Get DLB device $dlb"

pat='[0-9a-fA-F]+\:[0-9a-fA-F]+\.[0-9a-fA-F]+'
[[ $dlb =~ $pat ]]
pci_addr=${BASH_REMATCH[0]}
echo "DLB PCIe ID to be bind: $pci_addr"

echo "binding..."
$DPDK_DIR/usertools/dpdk-devbind.py --bind vfio-pci $pci_addr

echo "setup hugepages..."
$REPO_PATH/scripts/common/setup_hugepage.sh
