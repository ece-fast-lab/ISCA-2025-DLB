import pandas as pd
import re
import matplotlib.pyplot as plt
import numpy as np
import os

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")
reviewer_id = os.environ.get("REVIEWER_ID", "x")
user_password = os.environ.get("PASSWORD", "123456")

color_map = {
        'blue': '#4472C4',
        'orange': '#ED7D31',
        'yellow': '#FFC000',
        'green': '#70AD47',
    }

# Path to the log file
log_file_path = f"{repo_path}/scripts/fig11e/results_{reviewer_id}.log"

# Function to extract data from log file
def parse_log_file(file_path):
    data = {}
    with open(file_path, 'r') as file:
        content = file.read()
    
    # Splitting content into sections for DIR and LDB
    dir_sections = re.split(r'Running DIR PORTS', content)[1:]
    ldb_sections = re.split(r'Running LDB PORTS', content)[1:]
    
    for section in dir_sections[:8]:
        workers = re.search(r'NUM_WORKERS=(\d+)', section)
        throughput = re.search(r'Total worker throughput is (\d+\.\d+) Mpps', section)
        latency = re.search(r'Average worker latency is (\d+\.\d+) us', section)
        
        if workers and throughput and latency:
            worker_count = int(workers.group(1))
            data[worker_count] = {
                "DIR Port Throughput (Mpps)": float(throughput.group(1)),
                "DIR Port Avg Latency (us)": float(latency.group(1)),
                "LDB Port Throughput (Mpps)": None,
                "LDB Port Avg Latency (us)": None
            }
    
    for section in ldb_sections[:8]:
        workers = re.search(r'NUM_WORKERS=(\d+)', section)
        throughput = re.search(r'Total worker throughput is (\d+\.\d+) Mpps', section)
        latency = re.search(r'Average worker latency is (\d+\.\d+) us', section)
        
        if workers and throughput and latency:
            worker_count = int(workers.group(1))
            if worker_count not in data:
                data[worker_count] = {
                    "DIR Port Throughput (Mpps)": None,
                    "DIR Port Avg Latency (us)": None,
                    "LDB Port Throughput (Mpps)": float(throughput.group(1)),
                    "LDB Port Avg Latency (us)": float(latency.group(1))
                }
            else:
                data[worker_count]["LDB Port Throughput (Mpps)"] = float(throughput.group(1))
                data[worker_count]["LDB Port Avg Latency (us)"] = float(latency.group(1))
    
    df = pd.DataFrame.from_dict(data, orient='index')
    df.index.name = "Number of Workers"
    df = df.reset_index()
    return df

# Parse the log file
df = parse_log_file(log_file_path)

# Replace missing values ("None" or NaN) with 0 for plotting convenience
df = df.fillna(0)

# Prepare data for plotting
x = np.arange(len(df["Number of Workers"]))  # x positions for the groups
width = 0.4  # width of each bar

plt.rcParams.update({'font.size': 12})
fig, ax1 = plt.subplots(figsize=(8, 4))

# --- Left axis: Throughput (Mpps) ---
ax1.set_xlabel("# of Worker Cores")
ax1.set_ylabel("MPPS")

# Plot DIR and LDB throughput as side-by-side bars
bars_dir = ax1.bar(
    x - width/2, 
    df["DIR Port Throughput (Mpps)"], 
    width, 
    label="DIR Throughput", 
    color=color_map["blue"]
)
bars_ldb = ax1.bar(
    x + width/2, 
    df["LDB Port Throughput (Mpps)"], 
    width, 
    label="LDB Throughput", 
    color=color_map["green"]
)

ax1.set_xticks(x)
ax1.set_xticklabels(df["Number of Workers"])

# --- Right axis: Latency (us) ---
ax2 = ax1.twinx()
ax2.set_ylabel("Avg Latency (us)")

# Plot DIR and LDB latency as lines
line_dir, = ax2.plot(
    x, 
    df["DIR Port Avg Latency (us)"], 
    marker='o', 
    linestyle='-', 
    color=color_map["orange"], 
    label="DIR Latency", 
    zorder=3
)
line_ldb, = ax2.plot(
    x, 
    df["LDB Port Avg Latency (us)"], 
    marker='s', 
    linestyle='-', 
    color=color_map["yellow"], 
    label="LDB Latency", 
    zorder=3
)

# Make layout tighter
fig.tight_layout()

# Combine legends in a single box above the plot
fig.legend(
    [bars_dir, bars_ldb, line_dir, line_ldb], 
    ["DIR Throughput", "LDB Throughput", "DIR Latency", "LDB Latency"], 
    loc="upper center", 
    ncol=4, 
    bbox_to_anchor=(0.5, 1.12)
)

# Save to file
fig.savefig("fig11e.png", dpi=300, bbox_inches="tight")

# Uncomment to display the plot interactively:
# plt.show()
