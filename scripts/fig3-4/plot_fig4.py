import os
import re
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict
import pandas as pd
import glob

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/user/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "user")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "a")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

#!/usr/bin/env python3
import os
import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# 1) Regex to extract info from filenames: e.g., "dlb-ed_TX4_RX8.txt"
filename_re = re.compile(r'^(?P<tech>.+)_TX(?P<tx>\d+)_RX(?P<rx>\d+)\.txt$')

# 2) Regex to find throughput & latency in file text
re_total_throughput = re.compile(r'Total worker throughput is\s+([0-9.]+)\s+Mpps')
re_sample_throughput = re.compile(r'avg\s+([0-9.]+)\s+mpps', re.IGNORECASE)
re_worker_avg_latency = re.compile(r'Average worker latency is\s+([0-9.]+)\s+us')
re_avg_latency = re.compile(r'Average latency:\s*([0-9.]+)\s+us')
re_p99_latency = re.compile(r'99% tail latency:\s*([0-9.]+)\s+us')

def parse_file(filepath):
    """
    Parse a single results file and extract:
      - technology
      - TX (num of producer cores)
      - RX
      - throughput (Mpps)
      - average latency (us)
      - p99 latency (us)
    Returns a dict of these metrics, or None if not matched.
    """
    filename = os.path.basename(filepath)
    m = filename_re.match(filename)
    if not m:
        return None

    tech = m.group('tech')
    tx = int(m.group('tx'))
    rx = int(m.group('rx'))

    with open(filepath, 'r') as f:
        text = f.read()

    # Throughput: try "Total worker throughput" first; otherwise average sample lines
    match_total = re_total_throughput.search(text)
    if match_total:
        throughput = float(match_total.group(1))
    else:
        samples = [float(x) for x in re_sample_throughput.findall(text)]
        throughput = np.mean(samples) if samples else np.nan

    # Average latency: check for "Average worker latency" first
    match_worker_lat = re_worker_avg_latency.search(text)
    if match_worker_lat:
        avg_lat = float(match_worker_lat.group(1))
    else:
        lat_samples = [float(x) for x in re_avg_latency.findall(text)]
        avg_lat = np.mean(lat_samples) if lat_samples else np.nan

    # p99 latency: average all occurrences
    p99_samples = [float(x) for x in re_p99_latency.findall(text)]
    p99_lat = np.mean(p99_samples) if p99_samples else np.nan

    return {
        'file': filename,
        'technology': tech,
        'tx': tx,
        'rx': rx,
        'throughput_Mpps': throughput,
        'average_latency_us': avg_lat,
        'p99_latency_us': p99_lat
    }

def collect_results(results_folder='./results'):
    """
    Walk through the given folder and parse every '.txt' file.
    Returns a DataFrame of all parsed metrics.
    """
    records = []
    for fname in os.listdir(results_folder):
        if not fname.endswith('.txt'):
            continue
        filepath = os.path.join(results_folder, fname)
        parsed = parse_file(filepath)
        if parsed:
            records.append(parsed)
    return pd.DataFrame(records)

def plot_rx8_line_chart(df):
    """
    Plot a line chart of throughput vs. TX (i.e., # of producer cores)
    for all entries with RX=8. Each technology is its own line.
    """
    # Filter only rows where RX=8
    df_rx8 = df[(df['rx'] == 8) & ((df['technology'] == "dlb-lib") | (df['technology'] == "dlb-ed"))].copy()
    if df_rx8.empty:
        print("No data found for RX=8")
        return
    
    # Pivot so that index=TX, columns=technology, values=throughput
    pivot_df = df_rx8.pivot(index='tx', columns='technology', values='throughput_Mpps')
    pivot_df.sort_index(inplace=True)  # Ensure ascending TX order
    
    # Plot each technology as its own line
    plt.rcParams.update({'font.size': 18})
    fig, ax = plt.subplots(figsize=(8,4))
    
    # Define distinct markers for each technology (use more if you have >5 technologies)
    markers = ['o', 's', '^', 'D', 'v']
    for i, col in enumerate(pivot_df.columns):
        ax.plot(
            pivot_df.index,
            pivot_df[col],
            marker=markers[i % len(markers)],
            markersize=7,
            linewidth=2,
            label=col
        )
    
    ax.set_xlabel("# of Producer Cores")
    ax.set_ylabel("Throughput (MPPS)")
    # ax.set_title("Throughput vs. # of Producer Cores (RX=8)")
    ax.set_xticks(pivot_df.index)  # Show integer TX values on x-axis
    ax.grid(True, linestyle='--', color='gray', alpha=0.5)
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.25), ncol=2, frameon=False)

    # plt.grid(axis='y', linestyle='--', color='gray', alpha=0.5)
    # plt.legend(loc='upper center', bbox_to_anchor=(0.5, 1.15), ncol=2, frameon=False)
    
    plt.tight_layout()
    plt.savefig('fig4.png', dpi=300)
    # plt.show()


results_dir = f"results_{reviewer_id}"
df = collect_results(results_dir)
if df.empty:
    print("No result files found in './results'.")

# print("Parsed DataFrame:\n", df)

# Plot line chart for RX=8
plot_rx8_line_chart(df)



