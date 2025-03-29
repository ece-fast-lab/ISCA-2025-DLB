#!/bin/bash

set -e

sudo ./run_libdlb.sh

source ../common/setup_dpdk_dlb2.sh

sudo ./run_dpdk.sh
