import itertools
import os
import paramiko
import subprocess
import threading
import time

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/isca25_ae/ISCA-2025-DLB")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "isca25_ae")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
ssh_snic = os.environ.get("SNIC_SSH_IP", "192.17.100.19")     # Optionally read SNIC IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "x")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "123456")         # PASSWORD to use for sudo

dir_name = f"{repo_path}/scripts/fig9/results_{reviewer_id}/acc"
exe_path = f"{repo_path}/src/accdirect/offload-rdma-dlb"  # Path to the executable directory

# Configuration variables
server_threads_list = [8]
client_threads_list = range(2, 9)
num_events_list = [1000000]
window_size_list = [64]
# window_size_list = [64, 128, 256, 512, 1024]
msg_size_list = [64]

# SSH connection settings
server_hostname = ssh_host
server_username = ssh_user
server_password = user_password

snic_hostname = ssh_snic
snic_username = ssh_user
snic_password = user_password

# Create results directory if it doesn't exist
if not os.path.exists(dir_name):
    os.makedirs(dir_name)

# Function to execute command via SSH with output streaming to file and console
def ssh_command_execute(hostname, username, password, command, output_file):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(hostname, username=username, password=password)
    stdin, stdout, stderr = client.exec_command(command, get_pty=True)
    if (hostname == server_hostname):
        stdin.write(server_password + "\n")
        stdin.flush()
    with open(output_file, "w") as file:
        for line in iter(stdout.readline, ""):
            # print(line, end="")
            file.write(line)
    client.close()

# Function to execute local command with output streaming to file and console
def execute_local_command(command, output_file):
    process = subprocess.Popen(
        command,
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.PIPE,
        text=True  # Using text mode returns strings.
    )
    process.stdin.write(server_password + "\n")
    process.stdin.flush()
    with open(output_file, "w") as file:
        for line in iter(process.stdout.readline, ""):
            # print(line, end="")
            file.write(line)


def kill_remote_process(hostname, username, password, command):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(hostname, username=username, password=password)
    _, stdout, stderr = client.exec_command(command)
    output = stdout.read().decode()
    error = stderr.read().decode()
    if output:
        print("Output:", output)
    if error:
        print("Error:", error)
    client.close()

# Thread function for executing commands with detailed type description
def run_command(command, command_type, log_filename):
    print(f"Starting {command} command...")
    if command_type == "local":
        execute_local_command(command, log_filename)
    else:
        hostname, username, password = command_type
        ssh_command_execute(hostname, username, password, command, log_filename)
    # print(f"Completed {command_type} command.")

# Generate all combinations of configuration variables
combinations = itertools.product(
    server_threads_list, client_threads_list, num_events_list, window_size_list, msg_size_list
)

for f, d, n, w, s in combinations:
    print(f"\nRunning configuration: Threads={f}, Clients={d}, Events={n}, Window={w}, Msg Size={s}")
    
    run_dir = f"{dir_name}/f{f}_d{d}_n{n}_w{w}_s{s}"
    if not os.path.exists(run_dir):
        os.makedirs(run_dir)
    
    server_command = f"sudo {exe_path}/rdma_dlb_server -f {f} -d {d} -n {n} -w {w} -s {s}"
    snic_command = f"{exe_path}/rdma_dlb_snic -d {d} -n {n} -w {w} -s {s}"
    client_command = f"sudo -S {exe_path}/rdma_dlb_client -d {d} -n {n} -w {w} -s {s}"
    power_measure_command = f"sudo {repo_path}/scripts/fig9/power_measure.sh 5"

    # Start all commands simultaneously with log files
    server_thread = threading.Thread(target=run_command, args=(server_command, (server_hostname, server_username, server_password), f"{run_dir}/server_{d}_{w}_{s}.log"))
    snic_thread = threading.Thread(target=run_command, args=(snic_command, (snic_hostname, snic_username, snic_password), f"{run_dir}/snic_{d}_{w}_{s}.log"))
    client_thread = threading.Thread(target=run_command, args=(client_command, "local", f"{run_dir}/client_{d}_{w}_{s}.log"))
    power_thread = threading.Thread(target=run_command, args=(power_measure_command, (server_hostname, server_username, server_password), f"{run_dir}/power_{d}_{w}_{s}.log"))

    # Starting threads
    server_thread.start()
    time.sleep(3)
    snic_thread.start()
    time.sleep(5)
    client_thread.start()
    time.sleep(5)
    power_thread.start()

    # Joining threads to ensure all finish before moving on
    server_thread.join()
    # snic_thread.join()
    client_thread.join()
    power_thread.join()

    # print("Killing SNIC process remotely...")
    kill_snic_command = "pkill -f rdma_dlb_snic"
    kill_remote_process(snic_hostname, snic_username, snic_password, kill_snic_command)

    # print("All commands executed and their outputs have been printed and logged.")

print("All tasks completed and logged.")
