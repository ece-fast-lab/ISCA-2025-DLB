import re
import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

# Directory containing the results
directory = f"results_{reviewer_id}"
data = []

# Compile regex patterns for extracting data
tput_pattern = re.compile(r'Average of the highest three mpps values: (\d+\.\d+) mpps')

def extract_latency_from_section(file_path):
    with open(file_path, 'r') as file:
        lines = file.readlines()
    latency_section_start = next(i for i, line in enumerate(lines) if "==== E2E Latency ====" in line)
    avg_latency = float(lines[latency_section_start + 5].split(': ')[1].strip().split(' ')[0])
    percentile_25th = float(lines[latency_section_start + 7].split(': ')[1].strip().split(' ')[0])
    percentile_99th = float(lines[latency_section_start + 12].split(': ')[1].strip().split(' ')[0])
    
    # Find the throughput using regex pattern
    throughput_match = tput_pattern.search(' '.join(lines[latency_section_start + 15:latency_section_start + 20]))
    throughput = float(throughput_match.group(1)) if throughput_match else None
            
    return avg_latency, percentile_99th, percentile_25th, throughput

# Iterate through all files in the directory
for filename in os.listdir(directory):
    if filename.endswith(".log"):
        parts = filename.split('_')
        tech = parts[0]
        distribution = parts[-3]
        rate = int(parts[-1].split('.')[0].replace('r', '')) // 1000000  # Convert r1000000 to 1
        delay = parts[parts.index('d') + 1]
        rx_cores = int(parts[parts.index('rx') + 1])
        worker_cores = int(parts[parts.index('wlcores') + 1])
        size = parts[parts.index('size') + 1]
        # print(filename)
        
        avg, p99, p25, tput = extract_latency_from_section(os.path.join(directory, filename))
        
        data.append({
            'Technology': tech,
            'Packet Rate (MPPS)': rate,
            'Distribution': distribution,
            'Average Latency (us)': avg,
            '99th Percentile Latency (us)': p99,
            '25th Percentile Latency (us)': p25,
            'Delay': delay,
            'Size': size,
            'Actual Throughput (MPPS)': tput,
            'Number of RX Cores': rx_cores,
            'Number of Worker Cores': worker_cores
        })

# Create a DataFrame from the collected data
df = pd.DataFrame(data)

# Plotting
plt.rcParams.update({'font.size': 20})
plt.figure(figsize=(8, 4))
bar_width = 0.2

technologies = df['Technology'].unique()
technologies = ['dlb', 'rss', 'sw', 'distributor']
rates = sorted(df['Packet Rate (MPPS)'].unique())

new_df = df[(((df['Technology'] == 'sw') & (df['Packet Rate (MPPS)'] < 80)) |
    ((df['Technology'] == 'distributor') & (df['Packet Rate (MPPS)'] < 80)) |
    (df['Technology'] == 'rss') |
    (df['Technology'] == 'dlb') )
    & (df['99th Percentile Latency (us)'] < 10000)
    & (df['Delay'] == '100') 
    & (df['Distribution'] == 'exponential')]


# Plotting: x-axis is Actual Throughput and y-axis is 99th Percentile Latency,
# grouping by Technology. Each group is connected by a line.
plt.rcParams.update({'font.size': 20})
plt.figure(figsize=(8, 4))

color_map = {
        'distributor': '#4472C4',
        'dlb': '#ED7D31',
        'rss': '#FFC000',
        'sw': '#70AD47',
    }

# Group by technology and plot for each tech group.
for tech, group in new_df.groupby('Technology'):
    # Optionally, sort by throughput if order matters:
    group_sorted = group.sort_values('Packet Rate (MPPS)')
    plt.plot(group_sorted['Actual Throughput (MPPS)'].to_numpy(), 
         group_sorted['99th Percentile Latency (us)'].to_numpy(), 
         marker='o', linestyle='-', linewidth=2.5, label=tech, color=color_map[tech])
plt.xlabel('Throughput (MPPS)')
plt.ylabel('p99 Latency (us)')
# plt.title('99th Percentile Latency vs. Actual Throughput by Technology', fontsize=14)
# plt.legend(loc="upper right", ncol=2, frameon=False, fontsize='17')

labels_paper = ['dpdk-pd', 'dlb-ed', 'rss', 'dpdk-ed']
handles, labels = plt.gca().get_legend_handles_labels()
by_label = dict(zip(labels_paper, handles))

plt.legend(by_label.values(), by_label.keys(), loc="upper left", ncol=2, frameon=False, fontsize='17')
plt.ylim(0, 40)
plt.xlim(0, 60)
plt.grid(True)
plt.tight_layout()
plt.savefig('fig5.png')
# plt.show()