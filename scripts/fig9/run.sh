#!/bin/bash

# set -e

# Source environment variables from env_setup.sh
source ../common/env_setup.sh
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

python3 run_acc.py
python3 run_cpu.py
