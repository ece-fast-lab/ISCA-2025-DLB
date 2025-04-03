import pandas as pd
import re
import os
import matplotlib.pyplot as plt

dlb_colors = ['#004A86','#0095CA','#EDB200','#7D5BA6','#000F8A','#515A3D','#8BAE46','#E96115']
dlb_markers = ['o','s','X','v','^']
color_map = {
        'blue': '#4472C4',
        'orange': '#ED7D31',
        'yellow': '#FFC000',
        'green': '#70AD47',
    }

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

directory = f"{repo_path}/scripts/fig12/results_{reviewer_id}"

# Function to extract data from a single log file
def parse_packet_prio_log_file(file_path):
    data = {}    
    priority_pattern = re.compile(r'Packet priority (\d+) has (\d+) counts, ([\d.]+) us latency')
    
    with open(file_path, 'r') as file:
        content = file.read()
        matches = priority_pattern.findall(content)
        if not matches:
            print(f"No matches found in file: {file_path}")  # Debug: No matches found
        for match in matches:
            queue_priority = int(0)
            packet_priority = int(match[0])
            latency = float(match[2])
            key = (queue_priority, packet_priority)
            if key not in data:
                data[key] = []
            data[key].append(latency)
    
    return data

# Function to process multiple log files in a directory
def process_packet_prio_logs(directory, pattern="packet_prio_priority_tx8"):
    all_data = {}
    files_processed = 0  # Debug: Count files processed
    # Loop through all files in the directory
    for filename in os.listdir(directory):
        # if filename.startswith("packet_prio_priority_tx8") and filename.endswith(".txt"):
        if filename.startswith(pattern) and filename.endswith(".txt"):
            files_processed += 1
            file_path = os.path.join(directory, filename)
            file_data = parse_packet_prio_log_file(file_path)
            # Merge data from this file into the main dataset
            for key, latencies in file_data.items():
                if key not in all_data:
                    all_data[key] = []
                all_data[key].extend(latencies)
    
    if files_processed == 0:
        print("No files were processed. Check the directory path and file names.")  # Debug: No files processed
    
    # Calculate averages
    results = {}
    for key, latencies in all_data.items():
        if latencies:
            average_latency = sum(latencies) / len(latencies)
            results[key] = average_latency
        else:
            print(f"No data for {key}.")  # Debug: No data for key
    
    return results


# Function to extract data from a single log file
def parse_queue_prio_log_file(file_path):
    data = {}    
    priority_pattern = re.compile(r'Queue priority (\d+) has ([\d.]+) us latency')
    
    with open(file_path, 'r') as file:
        content = file.read()
        matches = priority_pattern.findall(content)
        if not matches:
            print(f"No matches found in file: {file_path}")  # Debug: No matches found
        for match in matches:
            queue_priority = int(match[0])
            packet_priority = int(0)
            latency = float(match[1])
            key = (queue_priority, packet_priority)
            if key not in data:
                data[key] = []
            data[key].append(latency)
    
    return data

# Function to process multiple log files in a directory
def process_queue_prio_logs(directory, pattern="queue_prio_priority_worker8"):
    all_data = {}
    files_processed = 0  # Debug: Count files processed
    # Loop through all files in the directory
    for filename in os.listdir(directory):
        # if filename.startswith("queue_prio_priority_worker8") and filename.endswith(".txt"):
        if filename.startswith(pattern) and filename.endswith(".txt"):
            files_processed += 1
            file_path = os.path.join(directory, filename)
            file_data = parse_queue_prio_log_file(file_path)
            # Merge data from this file into the main dataset
            for key, latencies in file_data.items():
                if key not in all_data:
                    all_data[key] = []
                all_data[key].extend(latencies)
    
    if files_processed == 0:
        print("No files were processed. Check the directory path and file names.")  # Debug: No files processed
    
    # Calculate averages
    results = {}
    for key, latencies in all_data.items():
        if latencies:
            average_latency = sum(latencies) / len(latencies)
            results[key] = average_latency
        else:
            print(f"No data for {key}.")  # Debug: No data for key
    
    return results


packet_prio_results = process_packet_prio_logs(directory)
queue_prio_results = process_queue_prio_logs(directory)

packet_priorities = []
packet_prio_avg_latency = []

queue_priorities = []
queue_prio_avg_latency = []

# Print results
for key, avg_latency in packet_prio_results.items():
    packet_priorities.append(key[1])
    packet_prio_avg_latency.append(avg_latency)
    # print(f"Queue priority {key[0]}, Packet priority {key[1]}: Average Latency = {avg_latency:.3f} us")
    
for key, avg_latency in queue_prio_results.items():
    queue_priorities.append(key[1])
    queue_prio_avg_latency.append(avg_latency)
    # print(f"Queue priority {key[0]}, Packet priority {key[1]}: Average Latency = {avg_latency:.3f} us")


# Creating a line plot for the same data
plt.rcParams.update({'font.size': 24})
plt.figure(figsize=(8, 5))

plt.plot(packet_priorities, queue_prio_avg_latency, marker='o', linestyle='-', linewidth=4, markersize=10, color=color_map["blue"], label='Queue Priority')
# plt.annotate(f'{queue_prio_avg_latency[6]:.0f}', (packet_priorities[6], 50),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["blue"])
# plt.annotate(f'{queue_prio_avg_latency[7]:.0f}', (packet_priorities[7], 50),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["blue"])

plt.plot(packet_priorities, packet_prio_avg_latency, marker='*', linestyle='-.', linewidth=4, markersize=10, color=color_map["orange"], label='Packet Priority')
# plt.annotate(f'{packet_prio_avg_latency[6]:.0f}', (packet_priorities[6], 47),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["orange"])
# plt.annotate(f'{packet_prio_avg_latency[7]:.0f}', (packet_priorities[7], 47),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["orange"])

plt.grid(axis='y', linestyle='--', color='gray', alpha=0.5)
plt.xlabel('Priority Level')
plt.ylabel('Average Latency (us)')
plt.xticks(packet_priorities)  # Ensure x-ticks match the packet priorities
plt.legend(fontsize=20, loc="upper left", frameon=False)
plt.ylim(0,30)
plt.tight_layout()
plt.savefig('fig12a.png')

# Show the plot
# plt.show()


### mix priority
# packet_prio_mix_latency = [19.90, 19.25, 33.25, 33.40, 86.95, 88.49, 4414.20, 4441.94]
# queue_prio_mix_latency = [6.34, 4.87, 8.39, 6.82, 11.17, 28.82, 1942.35, 7128.02]

packet_prio_mix_results = process_packet_prio_logs(directory, pattern="mix_prio_priority_tx8_worker1")
queue_prio_mix_results = process_queue_prio_logs(directory, pattern="mix_prio_priority_tx8_worker1")

packet_prio_mix_latency = []
queue_prio_mix_latency = []

# Print results
for key, avg_latency in packet_prio_mix_results.items():
    packet_prio_mix_latency.append(avg_latency)
    # print(f"Queue priority {key[0]}, Packet priority {key[1]}: Average Latency = {avg_latency:.3f} us")
    
for key, avg_latency in queue_prio_mix_results.items():
    queue_prio_mix_latency.append(avg_latency)
    # print(f"Queue priority {key[0]}, Packet priority {key[1]}: Average Latency = {avg_latency:.3f} us")

# Creating a line plot for the same data
plt.rcParams.update({'font.size': 24})
plt.figure(figsize=(8, 5))

plt.plot(packet_priorities, queue_prio_mix_latency, marker='o', linestyle='-', linewidth=4, markersize=10, color=color_map["blue"], label='Queue Priority')
# plt.annotate(f'{queue_prio_mix_latency[6]:.0f}', (packet_priorities[6], 100),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["blue"])
# plt.annotate(f'{queue_prio_mix_latency[7]:.0f}', (packet_priorities[7], 100),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["blue"])

plt.plot(packet_priorities, packet_prio_mix_latency, marker='*', linestyle='-.', linewidth=4, markersize=10, color=color_map["orange"], label='Packet Priority')
# plt.annotate(f'{packet_prio_mix_latency[6]:.0f}', (packet_priorities[6], 93),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["orange"])
# plt.annotate(f'{packet_prio_mix_latency[7]:.0f}', (packet_priorities[7], 93),
#              textcoords="offset points", xytext=(0,0), ha='center', fontsize=20, color=color_map["orange"])

plt.grid(axis='y', linestyle='--', color='gray', alpha=0.5)
plt.xlabel('Priority Level')
plt.ylabel('Average Latency (us)')
plt.xticks(packet_priorities)  # Ensure x-ticks match the packet priorities
plt.legend(fontsize=20, loc="upper left", frameon=False)
plt.ylim(0,100)
plt.tight_layout()
plt.savefig('fig12b.png')

# plt.grid(True)

# Show the plot
# plt.show()