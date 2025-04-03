import paramiko
import subprocess
import time
import re
import signal
import numpy as np
import threading
from itertools import product
import os

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

dir_path = f"{repo_path}/scripts/fig5/results_{reviewer_id}"

# Create directory for this combination if it doesn't exist
if not os.path.exists(dir_path):
    os.makedirs(dir_path)
    
# Define lists of options for remote command
remote_wlcores = ["16-23"]
remote_nb_rx_adapters = [5]
remote_e2e_latency = [1]

remote_c = ["0xffffffff"]
remote_s = ["0xfa"]
remote_a = ["0000:17:00.0"]
remote_test = ["perf_queue"]
remote_prod_type = ["ethdev"]
remote_stlist = ["p"]
remote_prod_enq_burst_sz = [128]
remote_worker_deq_depth = [128]


# Define lists of options for local command
local_l = ["0-13"]
local_size = [64]
local_delay = [100]
local_distribution = ["exponential"]
local_rate = [1000000, 5000000, 10000000, 15000000, 20000000, 25000000, 30000000, 35000000, 40000000, 45000000, 50000000, 60000000]
# local_rate = [45000000, 50000000, 60000000]
local_latency = [1000]


# Function to run remote command and capture mpps values with termination control
def run_remote_command(remote_command, mpps_values, stop_event, mpps_regex):
    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh_client.connect(ssh_host, username=ssh_user, password=user_password)

    # Execute the remote command
    ssh_stdin, ssh_stdout, ssh_stderr = ssh_client.exec_command(remote_command, get_pty=True)
    ssh_stdin.write(user_password + "\n")
    ssh_stdin.flush()
    print("Starting remote command...")
    for line in iter(ssh_stdout.readline, ""):
        if stop_event.is_set():  # Check if stop event is triggered
            break
        # print(line, end="")  # Print each line to terminal
        match = mpps_regex.search(line)
        if match:
            mpps = float(match.group(1))
            mpps_values.append(mpps)
    ssh_client.close()

def kill_remote_process(process_pattern):
    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh_client.connect(ssh_host, username=ssh_user, password=user_password)
    command = f"sudo pkill -f '{process_pattern}'"
    print(f"Executing remote pkill command: {command}")
    ssh_client.exec_command(command)
    ssh_client.close()

# Loop over each combination
for ii in range(4):
    # Generate all combinations of options
    all_combinations = product(
        remote_c, remote_s, remote_a, remote_test, remote_prod_type, remote_stlist,
        remote_prod_enq_burst_sz, remote_worker_deq_depth, remote_wlcores, remote_nb_rx_adapters,
        remote_e2e_latency, local_l, local_size, local_latency, local_delay, local_distribution, local_rate
    )

    if ii > 0:
        time.sleep(30)

    for (c, s, a, test, prod_type, stlist, enq_burst_sz, deq_depth, wlcores,
        nb_rx_adapters, e2e_latency, l, size, latency, d, D, rate) in all_combinations:
        
        # Calculate the number of wlcores by counting the range
        wlcores_count = int(wlcores.split('-')[1]) - int(wlcores.split('-')[0]) + 1
        local_l_count = int(l.split('-')[1]) - int(l.split('-')[0])
        
        nb_rx_adapters = 5

        
        # Define local command using the current combination
        local_command = (
            f'sudo -S timeout --signal=SIGINT 10s {repo_path}/src/dlb_bench/dpdk_bench/dpdk-tx/dpdk-tx -l 0-13 -a 0000:03:00.0 '
            f'-- --dest-mac=94:6d:ae:c7:77:3a --dest-ip=192.168.200.10 '
            f'--source-mac=b8:ce:f6:75:6c:d2 --source-ip=192.168.200.30 '
            f'--size={size} --latency={latency} -d {d} -D {D} --rate={rate}'
        )

        # List to capture mpps values from the remote command
        mpps_values = []
        stop_event = threading.Event()  # Event to control the termination of the remote command
        
        if (ii == 3):
            rss_remote_command = (
                f'sudo {repo_path}/src/dlb_bench/dpdk_bench/dpdk-rx/dpdk-rx -l 0-{0+wlcores_count+nb_rx_adapters} -a {a} '
                f'-- --latency=1000 -m 0'
            )
            process_pattern = "dpdk-rx"
            
            logfile_path = f'{dir_path}/rss_rx_{nb_rx_adapters}_wlcores_{wlcores_count}_size_{size}_tx_{local_l_count}_d_{d}_{D}_r_{rate}.log'
            # Start the remote command in a separate thread
            mpps_regex = re.compile(r"Total RX packet per second: ([\d.]+) M")
            remote_thread = threading.Thread(target=run_remote_command, args=(rss_remote_command, mpps_values, stop_event, mpps_regex))

        elif (ii == 0):
            dlb_remote_command = (
                f'sudo {repo_path}/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2/build/app/dpdk-test-eventdev -c {c} -s {s}  -a {a} '
                f'-a 0000:f5:00.0 -- --test={test} --prod_type_{prod_type} '
                f'--stlist={stlist} --prod_enq_burst_sz={enq_burst_sz} '
                f'--worker_deq_depth={deq_depth} --wlcores={wlcores} '
                f'--nb_rx_adapters={nb_rx_adapters} --e2e_latency={e2e_latency}'
            )
            process_pattern = "dpdk-test-eventdev"
            
            logfile_path = f'{dir_path}/dlb_rx_{nb_rx_adapters}_wlcores_{wlcores_count}_size_{size}_tx_{local_l_count}_d_{d}_{D}_r_{rate}.log'
            # Start the remote command in a separate thread
            mpps_regex = re.compile(r"sample \d+: ([\d.]+) mpps")
            remote_thread = threading.Thread(target=run_remote_command, args=(dlb_remote_command, mpps_values, stop_event, mpps_regex))
            
            
        elif (ii == 1):
            # deq_depth = 32
            sw_remote_command = (
                f'sudo {repo_path}/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2/build/app/dpdk-test-eventdev -c {c} -s {s} -a {a} '
                f'--vdev=event_sw0 -- --test={test} --prod_type_{prod_type} '
                f'--stlist={stlist} --prod_enq_burst_sz={enq_burst_sz} '
                f'--worker_deq_depth={deq_depth} --wlcores={wlcores} '
                f'--nb_rx_adapters={nb_rx_adapters} --e2e_latency={e2e_latency}'
            )
            process_pattern = "dpdk-test-eventdev"
            
            logfile_path = f'{dir_path}/sw_rx_{nb_rx_adapters}_wlcores_{wlcores_count}_size_{size}_tx_{local_l_count}_d_{d}_{D}_r_{rate}.log'
            # Start the remote command in a separate thread
            mpps_regex = re.compile(r"sample \d+: ([\d.]+) mpps")
            remote_thread = threading.Thread(target=run_remote_command, args=(sw_remote_command, mpps_values, stop_event, mpps_regex))
            
        elif (ii == 2):
            distributor_remote_command = (
                f'sudo {repo_path}/src/dpdk-22.11.2-dlb/dpdk-stable-22.11.2/build/app/dpdk-distributor-e2e -l 0-{wlcores_count+nb_rx_adapters+1} -a {a} '
                f'-- -p 1 -w {wlcores_count} -r {nb_rx_adapters}'
            )
            process_pattern = "dpdk-distributor-e2e"
            
            logfile_path = f'{dir_path}/distributor_rx_{nb_rx_adapters}_wlcores_{wlcores_count}_size_{size}_tx_{local_l_count}_d_{d}_{D}_r_{rate}.log'
            # Start the remote command in a separate thread
            mpps_regex = re.compile(r"\s*-\s*Sent:\s+(\d+(?:\.\d+)?)")
            remote_thread = threading.Thread(target=run_remote_command, args=(distributor_remote_command, mpps_values, stop_event, mpps_regex))
            
        else:
            continue
        
        remote_thread.start()
        time.sleep(6)

        # Run the local command and log its output while the remote command is running
        with open(logfile_path, 'w') as logfile:
            print("Starting local command...")
            local_process = subprocess.Popen(local_command.split(), stdin=subprocess.PIPE, stdout=logfile, stderr=logfile, preexec_fn=os.setsid)
            local_process.stdin.write(f"{user_password}\n".encode())
            local_process.stdin.flush()
            print(local_process.pid)

            # Allow the local command to run for a specified time (10 seconds)
            # time.sleep(10)

            # Attempt to terminate the specific process using pkill as a fallback
            # subprocess.run(['sudo', 'pkill', '-f', './dpdk-tx'], check=True)
            local_process.communicate()

        time.sleep(6)

        kill_remote_process(process_pattern)
        # Signal the remote thread to stop and wait for it to finish
        stop_event.set()
        remote_thread.join()


        # Calculate and log the average of the highest three mpps values
        if len(mpps_values) >= 3:
            top_three_mpps = sorted(mpps_values, reverse=True)[:3]
            average_top_three = np.mean(top_three_mpps)
            with open(logfile_path, 'a') as logfile:
                logfile.write(f"\nAverage of the highest three mpps values: {average_top_three:.3f} mpps\n")
            print(f"Average mpps appended to {logfile_path}.")
        else:
            print("Not enough mpps samples to calculate the top three average.")
            
        time.sleep(6)
    
