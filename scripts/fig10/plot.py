import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import itertools
import re
import os

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo


# Regular expression pattern to match avgRPS value
total_rps_pattern = re.compile(r"total RPS\s*=\s*(\d+)")


def extract_total_rps(log_file_path, threshold=0.4):
    thread_rps = []
    with open(log_file_path, 'r') as f:
        for line in f:
            match = re.search(r"Thread\s+\d+:\s+\d+\s+iters\s+in\s+[\d\.]+\s+seconds,\s+rps\s*=\s*([\d\.]+)", line)
            if match:
                rps_val = float(match.group(1))
                thread_rps.append(rps_val)
    
    if not thread_rps:
        return None
    
    max_rps = max(thread_rps)
    if any(rps < threshold * max_rps for rps in thread_rps):
        return None

    with open(log_file_path, "r") as file:
        content = file.read()
        match = total_rps_pattern.search(content)
        if match:
            return float(match.group(1))
    return None


def extract_latencies(file_path):
    latencies = []
    with open(file_path, "r") as file:
        for line in file:
            try:
                latency = float(line.strip())
                latencies.append(latency)
            except ValueError:
                continue
    return latencies


def compute_statistics(latencies):
    if not latencies:
        return {"avg": None, "median": None, "p95": None, "p99": None}

    latencies = np.array(latencies)
    avg = np.mean(latencies)
    median = np.median(latencies)
    p95 = np.percentile(latencies, 95)
    p99 = np.percentile(latencies, 99)

    return {"avg": avg, "median": median, "p95": p95, "p99": p99}


def iterate_experiment_directories(base_dir):
    data = []
    for entry in os.listdir(base_dir):
        entry_path = os.path.join(base_dir, entry)
        if os.path.isdir(entry_path):
            # Process log.txt file
            log_file_path = os.path.join(entry_path, "log.txt")
            total_rps = None
            if os.path.isfile(log_file_path):
                total_rps = extract_total_rps(log_file_path)

            if total_rps is None:
                continue

            # Process .result file
            result_file_path = None
            for file_name in os.listdir(entry_path):
                if file_name.endswith(".result"):
                    result_file_path = os.path.join(entry_path, file_name)
                    break

            latencies = []
            if result_file_path and os.path.isfile(result_file_path):
                latencies = extract_latencies(result_file_path)

            stats = compute_statistics(latencies)

            if total_rps is not None:
                config = parse_directory_name(entry)
                if config:
                    config.update({"RPS": total_rps, **stats})
                    data.append(config)

    return data

def group_data(df, group_size=5):
    df = df[(df['p99'] < 200) & (df['window_size'] < 60)]
    df_sorted = df.sort_values(by='MRPS').reset_index(drop=True)
    grouped = df_sorted.groupby(df_sorted.index // group_size).agg({'MRPS': 'mean', 'p99': 'mean'}).reset_index(drop=True)
    return grouped

def parse_directory_name(directory_name):
    pattern = re.compile(
        r"k(?P<k>\d+)_w(?P<window_size>\d+)_j(?P<thread_num>\d+)_r(?P<ratio>\d+)_p(?P<percent>\d+)"
    )
    match = pattern.match(directory_name)
    if match:
        return match.groupdict()
    return None


# put (1x), get (1x), scan2 (2x), scan16 (5x), scan40 (10x), scan64 (15x), scan95 (20x), scan115 (25x)
dist_list = ["put", "get", "scan3", "scan18", "scan46", "scan75", "scan105"]
config_dict = {50: [dist_list[1], dist_list[0]], 33: [dist_list[1], dist_list[6], dist_list[4]],
               20: [dist_list[1], dist_list[6]], 15: [dist_list[1], dist_list[5]],
               10: [dist_list[1], dist_list[4]], 5: [dist_list[1], dist_list[3]],
               2: [dist_list[1], dist_list[2]]}


dlb_data = iterate_experiment_directories(f"{repo_path}/scripts/fig10/results_{reviewer_id}/dlb")
baseline_data = iterate_experiment_directories(f"{repo_path}/scripts/fig10/results_{reviewer_id}/rss")
dpdk_pd_data = iterate_experiment_directories(f"{repo_path}/scripts/fig10/results_{reviewer_id}/dpdk-pd")

dlb = pd.DataFrame(dlb_data)
baseline = pd.DataFrame(baseline_data)
dpdk_pd = pd.DataFrame(dpdk_pd_data)

# Convert RPS to MRPS (Mega Requests Per Second)
dlb['MRPS'] = dlb['RPS'] / 1e6
baseline['MRPS'] = baseline['RPS'] / 1e6
dpdk_pd['MRPS'] = dpdk_pd['RPS'] / 1e6

dlb['window_size'] = dlb['window_size'].astype(int)
baseline['window_size'] = baseline['window_size'].astype(int)
dpdk_pd['window_size'] = dpdk_pd['window_size'].astype(int)

dlb['thread_num'] = dlb['thread_num'].astype(int)
baseline['thread_num'] = baseline['thread_num'].astype(int)
dpdk_pd['thread_num'] = dpdk_pd['thread_num'].astype(int)

print(dlb.sort_values(by=['window_size']))
print(baseline.sort_values(by=['window_size']))
print(dpdk_pd.sort_values(by=['window_size']))

unique_combinations = baseline[['ratio', 'percent']].drop_duplicates().reset_index(drop=True)
# print(unique_combinations)

for index, row in unique_combinations.iterrows():
    # Access elements of the row for 'ratio' and 'percent'
    current_ratio = row['ratio']
    current_percent = row['percent']
    # print(current_ratio, current_percent)
    
    dlb_f = dlb
    baseline_f = baseline
    dpdk_pd_f = dpdk_pd

    dlb_f = dlb_f.sort_values(by=['MRPS'])
    baseline_f = baseline_f.sort_values(by=['MRPS'])
    dpdk_pd_f = dpdk_pd_f.sort_values(by=['MRPS'])

    group_size = 1  # Adjust group size as needed for smoother curves
    baseline_f = group_data(baseline_f, group_size)
    dpdk_pd_f = group_data(dpdk_pd_f, group_size)
    dlb_f = group_data(dlb_f, group_size)

    # Plot RPS vs p99 latency
    plt.figure(figsize=(8, 5))
    plt.rcParams.update({'font.size': 22})
    plt.plot(baseline_f['MRPS'].values, baseline_f['p99'].values, marker='o', linestyle='-', label='rss', color='#004A86')
    plt.plot(dpdk_pd_f['MRPS'].values, dpdk_pd_f['p99'].values, marker='o', linestyle='-', label='swlb', color='green')
    plt.plot(dlb_f['MRPS'].values, dlb_f['p99'].values, marker='o', linestyle='-', label='acc-dlb-lib', color='#E96115')
    plt.xlabel('MRPS')
    plt.ylabel('p99 Latency (us)')
    # plt.legend(loc="upper left", ncol=1, frameon=True, fontsize='17')
    plt.legend(
        loc="upper center", 
        ncol=3, 
        bbox_to_anchor=(0.5, 1.25),
        frameon=True
        )
    plt.ylim(0, 60)
    plt.tight_layout()
    plt.savefig(f'fig10.png')
