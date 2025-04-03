import pandas as pd
import os
import matplotlib.pyplot as plt

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
ssh_snic = os.environ.get("SNIC_SSH_IP", "192.17.100.19")     # Optionally read SNIC IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

base_directory = f"{repo_path}/scripts/fig9/results_{reviewer_id}/acc"
cpu_base_directory = f"{repo_path}/scripts/fig9/results_{reviewer_id}/cpu"

color_map = {
        'blue': '#4472C4',
        'orange': '#ED7D31',
        'yellow': '#FFC000',
        'green': '#70AD47',
    }

# Initialize the DataFrame with appropriate columns
columns = ['Configuration', 'Server Threads', 'Client Threads', 'Number of Events', 'Window Size', 'Message Size',
           'Average Latency (us)', 'P50 Latency (us)', 'P99 Latency (us)', 'Request Rate (mrps)', 'Average Power (Watts)']
results_df = pd.DataFrame(columns=columns)
cpu_results_df = pd.DataFrame(columns=columns)

# Helper function to extract metrics from client logs
def parse_client_log(filepath):
    with open(filepath, 'r') as file:
        lines = file.readlines()
    request_rate = float([line for line in lines if "Request rate is" in line][-1].split()[-2])
    latencies = {line.split(':')[0].strip(): float(line.split()[-2]) for line in lines if '%' in line}
    average_latency = float([line for line in lines if "Average latency:" in line][0].split()[-2])
    return average_latency, latencies['50% tail latency'], latencies['99% tail latency'], request_rate

# Helper function to extract average power from power logs
def parse_power_log(filepath):
    with open(filepath, 'r') as file:
        lines = file.readlines()
    average_power = float([line for line in lines if "Average power reading" in line][0].split()[-2])
    return average_power

# Function to extract configuration from folder names
def extract_config_from_folder_name(folder_name):
    parts = folder_name.split('_')
    return {
        'f': int(parts[0][1:]),
        'd': int(parts[1][1:]),
        'n': int(parts[2][1:]),
        'w': int(parts[3][1:]),
        's': int(parts[4][1:])
    }

# Iterate over each configuration folder and compile results
for config_dir in os.listdir(base_directory):
    # print(config_dir)
    config_path = os.path.join(base_directory, config_dir)
    config = extract_config_from_folder_name(config_dir)
    
    # Ensure that client and power logs exist
    client_log = [f for f in os.listdir(config_path) if 'client' in f][0]
    power_log = [f for f in os.listdir(config_path) if 'power' in f][0]
    
    # Parse logs
    avg_latency, p50_latency, p99_latency, request_rate = parse_client_log(os.path.join(config_path, client_log))
    avg_power = parse_power_log(os.path.join(config_path, power_log))
    
    # Prepare new row
    new_row = pd.DataFrame({
        'Configuration': [config_dir],
        'Server Threads': [config['f']],
        'Client Threads': [config['d']],
        'Number of Events': [config['n']],
        'Window Size': [config['w']],
        'Message Size': [config['s']],
        'Average Latency (us)': [avg_latency],
        'P50 Latency (us)': [p50_latency],
        'P99 Latency (us)': [p99_latency],
        'Request Rate (mrps)': [request_rate],
        'Average Power (Watts)': [avg_power]
    }, columns=columns)
    results_df = pd.concat([results_df, new_row], ignore_index=True)

results_df.to_csv('snic-dlb.csv', index=False) 


for config_dir in os.listdir(cpu_base_directory):
    config_path = os.path.join(cpu_base_directory, config_dir)
    config = extract_config_from_folder_name(config_dir)
    
    # Ensure that client and power logs exist
    client_log = [f for f in os.listdir(config_path) if 'client' in f][0]
    power_log = [f for f in os.listdir(config_path) if 'power' in f][0]
    
    # Parse logs
    avg_latency, p50_latency, p99_latency, request_rate = parse_client_log(os.path.join(config_path, client_log))
    avg_power = parse_power_log(os.path.join(config_path, power_log))
    
    # Prepare new row
    new_row = pd.DataFrame({
        'Configuration': [config_dir],
        'Server Threads': [config['f']],
        'Client Threads': [config['d']],
        'Number of Events': [config['n']],
        'Window Size': [config['w']],
        'Message Size': [config['s']],
        'Average Latency (us)': [avg_latency],
        'P50 Latency (us)': [p50_latency],
        'P99 Latency (us)': [p99_latency],
        'Request Rate (mrps)': [request_rate],
        'Average Power (Watts)': [avg_power]
    }, columns=columns)
    cpu_results_df = pd.concat([cpu_results_df, new_row], ignore_index=True)
    
cpu_results_df.to_csv('cpu-dlb.csv', index=False) 


results_df = pd.read_csv('snic-dlb.csv')
cpu_results_df = pd.read_csv('cpu-dlb.csv')


results_df['Configuration'] = results_df['Configuration'].str.replace('_n1000000', '', regex=False)
cpu_results_df['Configuration'] = cpu_results_df['Configuration'].str.replace('_n1000000', '', regex=False)

results_df['Energy per Request (Joules)'] = results_df['Average Power (Watts)'] / results_df['Request Rate (mrps)']
cpu_results_df['Energy per Request (Joules)'] = cpu_results_df['Average Power (Watts)'] / cpu_results_df['Request Rate (mrps)']

results_df = results_df[(results_df['Window Size'] == 64) & (results_df['Client Threads'] < 10)]
cpu_results_df = cpu_results_df[(cpu_results_df['Window Size'] == 64) & (cpu_results_df['Client Threads'] < 10)]

lat_results_df = results_df.groupby('Client Threads').agg({
    'P99 Latency (us)': 'mean',
    'Request Rate (mrps)': 'mean',
    'Average Power (Watts)': 'mean',
    'Energy per Request (Joules)': 'mean'
}).reset_index()
# Rename columns for clarity
lat_results_df.columns = ['Client Threads', 'Average P99 Latency (us)', 'Average Average MRPS', 'Average Average Power (Watts)', 'Average Energy per Request (Joules)']

lat_cpu_results_df = cpu_results_df.groupby('Client Threads').agg({
    'P99 Latency (us)': 'mean',
    'Request Rate (mrps)': 'mean',
    'Average Power (Watts)': 'mean',
    'Energy per Request (Joules)': 'mean'
}).reset_index()
# Rename columns for clarity
lat_cpu_results_df.columns = ['Client Threads', 'Average P99 Latency (us)', 'Average Average MRPS', 'Average Average Power (Watts)', 'Average Energy per Request (Joules)']


merged_df = pd.merge(results_df, cpu_results_df, on=['Configuration', 'Client Threads'], suffixes=('_df1', '_df2'))
merged_df['Power Difference'] = merged_df['Average Power (Watts)_df2'] - merged_df['Average Power (Watts)_df1']
merged_df['Average MRPS'] = (merged_df['Request Rate (mrps)_df1'] + merged_df['Request Rate (mrps)_df2']) / 2

averages_df = merged_df.groupby('Client Threads').agg({
    'Power Difference': 'mean',
    'Average MRPS': 'mean',
    'Average Power (Watts)_df1': 'mean',
    'Average Power (Watts)_df2': 'mean'
}).reset_index()

# Rename columns for clarity
averages_df.columns = ['Client Threads', 'Average Power Difference', 'Average Average MRPS', 'Power_df1', 'Power_df2']


# Display the final DataFrame
# print(averages_df)

# print('average mrps', list(averages_df['Average Average MRPS']))
# print('power difference', list(averages_df['Average Power Difference']))
# print('snic-dlb p99', list(lat_results_df['Average P99 Latency (us)']))
# print('cpu-nic p99', list(lat_cpu_results_df['Average P99 Latency (us)']))
# print('snic-dlb power', list(averages_df['Power_df1']))
# print('cpu-nic power', list(averages_df['Power_df2']))


plt.rcParams.update({'font.size': 20})
plt.figure(figsize=(8, 5))
plt.scatter(averages_df['Average Average MRPS'], averages_df['Average Power Difference'], color='blue')

plt.plot(list(averages_df['Average Average MRPS']), list(averages_df['Average Power Difference']), color='blue', label='power saving')

plt.xlabel('Request Rate (MRPS)')
plt.ylabel('Power Saving (Watts)')
plt.legend(
    loc='upper center', borderaxespad=0.,
    frameon=False, ncol=2
)
# plt.legend()
plt.grid(True, which='both', linestyle='--', linewidth=0.5, axis='y')
plt.tight_layout()
plt.savefig('fig9_power.png')



plt.rcParams.update({'font.size': 20})
fig, ax1 = plt.subplots(figsize=(8, 5))
ax2 = ax1.twinx()  # Create a second y-axis sharing the same x-axis

# Plot bar chart on the left axis
averages_sorted = averages_df.sort_values(by='Average Power Difference', ascending=False)

# Plot each bar individually so that the highest bar is drawn in the back (with a lower zorder)
for i, (_, row) in enumerate(averages_sorted.iterrows()):
    ax1.bar(
        row['Average Average MRPS'],
        row['Average Power Difference'],
        width=0.5,
        color=color_map['blue'],
        edgecolor='black',  # Set border color
        linewidth=1.5,      # Set border line width
        zorder=1,
        label='power saving' if i == 0 else ""
    )

# Plot scatter plots on the right axis
ax2.scatter(
    averages_df['Average Average MRPS'],
    lat_results_df['Average P99 Latency (us)'], 
    color=color_map['orange'],
    marker='^', s=80,
    label='acc-dlb-lib'
)
ax2.scatter(
    averages_df['Average Average MRPS'],
    lat_cpu_results_df['Average P99 Latency (us)'], 
    facecolor="None",
    edgecolors=color_map['green'], linewidth=2,
    marker='o', s=80, 
    label='dlb-lib'
)

# ax2.plot(
#     list(averages_df['Average Average MRPS']),
#     list(lat_results_df['Average P99 Latency (us)']), 
#     color=color_map['orange'],
# )
# ax2.plot(
#     list(averages_df['Average Average MRPS']),
#     list(lat_cpu_results_df['Average P99 Latency (us)']), 
#     color=color_map['green'],
# )


ax1.set_xlabel('Request Rate (MRPS)')
ax1.set_ylabel('Power Saving (Watts)')
ax2.set_ylabel('P99 Latency (us)')

h1, l1 = ax1.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax1.legend(h1 + h2, l1 + l2, loc='upper center', borderaxespad=0., frameon=False, ncol=3, bbox_to_anchor=(0.5, 1.18))

ax2.set_ylim(16, 32)

ax1.grid(True, which='both', linestyle='--', linewidth=0.5, axis='y')

plt.tight_layout()
plt.savefig('fig9.png')
