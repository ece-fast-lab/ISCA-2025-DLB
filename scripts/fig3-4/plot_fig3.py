#!/usr/bin/env python3
import os
import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo


# Regex to extract technology, TX, RX from filenames, e.g. "dpdk-ed_TX8_RX4.txt"
filename_re = re.compile(r'^(?P<tech>.+)_TX(?P<tx>\d+)_RX(?P<rx>\d+)\.txt$')

# Regexes to parse throughput and latency lines in the file
re_total_throughput = re.compile(r'Total worker throughput is\s+([0-9.]+)\s+Mpps')
re_sample_throughput = re.compile(r'avg\s+([0-9.]+)\s+mpps', re.IGNORECASE)
re_worker_avg_latency = re.compile(r'Average worker latency is\s+([0-9.]+)\s+us')
re_avg_latency = re.compile(r'Average latency:\s*([0-9.]+)\s+us')
re_p99_latency = re.compile(r'99% tail latency:\s*([0-9.]+)\s+us')

# Special regex for dpdk-pd: use "Enqueued:" value as throughput.
re_enqueued = re.compile(r'-\s+Enqueued:\s+([0-9.]+)')

def parse_file(filepath):
    """
    Parse a single file to extract:
      - technology (from filename)
      - tx and rx values (from filename)
      - throughput (Mpps)
      - average latency (us)
      - p99 latency (us)
    
    For most files, throughput is obtained from either the "Total worker throughput" line or 
    averaging sample "avg ... mpps" values. However, for files with technology "dpdk-pd",
    throughput is computed as the average of the "Enqueued:" values (dropping the first and last measurements).
    """
    fname = os.path.basename(filepath)
    m = filename_re.match(fname)
    if not m:
        return None

    tech = m.group('tech')
    tx = int(m.group('tx'))
    rx = int(m.group('rx'))

    with open(filepath, 'r') as f:
        text = f.read()

    # Throughput extraction: use special logic if technology is "dpdk-pd"
    if tech == "dpdk-pd":
        enqueued_vals = [float(x) for x in re_enqueued.findall(text)]
        if len(enqueued_vals) > 2:
            # Drop the first and the last measurement
            enqueued_vals = enqueued_vals[1:-1]
        throughput = np.mean(enqueued_vals) if enqueued_vals else np.nan
    else:
        # For other technologies, try overall throughput first, else average sample throughput lines.
        m_total = re_total_throughput.search(text)
        if m_total:
            throughput = float(m_total.group(1))
        else:
            samples = [float(x) for x in re_sample_throughput.findall(text)]
            throughput = np.mean(samples) if samples else np.nan

    # Latency: prefer "Average worker latency" if available, else average all "Average latency:" values.
    m_worker_lat = re_worker_avg_latency.search(text)
    if m_worker_lat:
        avg_latency = float(m_worker_lat.group(1))
    else:
        lat_samples = [float(x) for x in re_avg_latency.findall(text)]
        avg_latency = np.mean(lat_samples) if lat_samples else np.nan

    # p99 latency: average all occurrences of "99% tail latency:" values.
    p99_samples = [float(x) for x in re_p99_latency.findall(text)]
    p99_latency = np.mean(p99_samples) if p99_samples else np.nan

    return {
        'file': fname,
        'technology': tech,
        'tx': tx,
        'rx': rx,
        'throughput_Mpps': throughput,
        'average_latency_us': avg_latency,
        'p99_latency_us': p99_latency
    }

def collect_results(results_folder='./results'):
    """
    Walk through the given folder, parse each .txt file, and return a DataFrame with the extracted metrics.
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




results_folder = f"./results_{reviewer_id}"

# 1) Collect all results into a DataFrame
df = collect_results(results_folder)
if df.empty:
    print("No .txt result files found.")

# 2) Filter for TX=8
df_tx8 = df[df['tx'] == 8].copy()
if df_tx8.empty:
    print("No data found for TX=8.")

# 3) Sort out the distinct worker-core counts (rx) and technologies
rx_values = sorted(df_tx8['rx'].unique())
tech_values = sorted(df_tx8['technology'].unique())

# If you want a specific order for known technologies, e.g.:
# tech_order = ['dpdk-pd', 'dpdk-ed', 'dlb-ed', 'dlb-lib']
# We'll reorder only if these exist in the data
tech_order = [t for t in ['dpdk-pd', 'dpdk-ed', 'dlb-ed', 'dlb-lib'] if t in tech_values]
# Include any other technologies that might appear, in alphabetical order
for t in tech_values:
    if t not in tech_order:
        tech_order.append(t)

# 4) Prepare the figure: grouped bars for throughput, lines for latencies
fig, ax1 = plt.subplots(figsize=(8, 4))
ax2 = ax1.twinx()  # second y-axis for latency

# We'll create a group of bars for each RX. The group center is at x_positions[i].
x_positions = np.arange(len(rx_values))
group_width = 0.8  # total width for all bars in a group
bar_width = group_width / len(tech_order)


color_map = {
        'dpdk-pd': '#4472C4',
        'dlb-ed': '#ED7D31',
        'dlb-lib': '#FFC000',
        'dpdk-ed': '#70AD47',
    }

# Plot bars for each technology
bar_handles = []
for i, tech in enumerate(tech_order):
    # For each rx, find the throughput for (tech, rx)
    y_vals = []
    for rx in rx_values:
        row = df_tx8[(df_tx8['rx'] == rx) & (df_tx8['technology'] == tech)]
        if len(row) == 1:
            y_vals.append(row['throughput_Mpps'].values[0])
        else:
            # If no data for that (tech, rx), put 0 or np.nan
            y_vals.append(np.nan)
    
    # Shift each technology's bar horizontally within the group
    bar_x = x_positions + (i - (len(tech_order)-1)/2) * bar_width
    # Plot
    h = ax1.bar(
        bar_x, y_vals, width=bar_width, label=tech, color=color_map[tech]
    )
    bar_handles.append(h[0])  # keep the handle for the legend

# 5) For the latencies, compute the *average across all technologies* per RX
#    (If you want separate lines per technology, you can adapt accordingly.)
latency_means = df_tx8.groupby('rx')[['average_latency_us','p99_latency_us']].mean()
avg_lat_vals = latency_means['average_latency_us'].reindex(rx_values).values
p99_vals = latency_means['p99_latency_us'].reindex(rx_values).values

# Plot them as lines on the second y-axis
# line_avg, = ax2.plot(
#     x_positions, avg_lat_vals, marker='o', color='dimgray', label='avg. lat.'
# )
# line_p99, = ax2.plot(
#     x_positions, p99_vals, marker='s', color='gray', label='p99'
# )

# 6) Label axes, ticks, etc.
ax1.set_xticks(x_positions)
ax1.set_xticklabels(rx_values)
ax1.set_xlabel('# of Worker Cores')
ax1.set_ylabel('Throughput (MPPS)')
ax2.set_ylabel('Latency (us)')

# Optionally set axis limits to resemble your sample figure
# ax1.set_ylim(0, 110)
# ax2.set_ylim(0, 900)

# 7) Build a combined legend from both axes
handles_bars, labels_bars = ax1.get_legend_handles_labels()
handles_lines, labels_lines = ax2.get_legend_handles_labels()

# Combine them
handles = handles_bars + handles_lines
labels = labels_bars + labels_lines

# Filter out any that start with '_'
filtered = [(h, l) for (h, l) in zip(handles, labels) if not l.startswith('_')]

if filtered:  # Make sure there's something to show
    ax1.legend(*zip(*filtered), loc='upper center', bbox_to_anchor=(0.5, 1.15), ncol=6, frameon=False)

# plt.title('Throughput & Latency vs. # of Worker Cores (TX=8)')
plt.tight_layout()
# plt.show()
plt.savefig('fig3.png')