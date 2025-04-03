#!/bin/bash

# set -e

python3 run_dpdk-pd.py
python3 run_baseline.py
python3 run_dlb.py
