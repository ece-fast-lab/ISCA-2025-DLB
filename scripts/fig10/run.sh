#!/bin/bash

# set -e

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

python3 run_dpdk-pd.py
python3 run_baseline.py
python3 run_dlb.py
