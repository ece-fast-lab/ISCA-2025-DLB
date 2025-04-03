import re
import subprocess
import time
import itertools
import os

import paramiko

# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/jiaqil6/ISCA-2025-DLB-draft")  # Repository path from env_setup.sh
ssh_user = os.environ.get("HOST_ACCOUNT", "jiaqil6")         # Use HOST_ACCOUNT from env_setup.sh
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")     # Optionally read server IP from env
ssh_snic = os.environ.get("SNIC_SSH_IP", "192.17.100.19")     # Optionally read SNIC IP from env
reviewer_id = os.environ.get("REVIEWER_ID", "b")         # REVIEWER_ID to determine directory name
user_password = os.environ.get("PASSWORD", "1234!@#$")         # PASSWORD to use for sudo

dir_name = f"{repo_path}/scripts/fig10/results_{reviewer_id}/dlb"

exe_path = f"{repo_path}/src/masstree"  # Path to the executable directory

# Configurable variables
server_threads_list = [16]
client_threads_list = [i for i in range(2, 10)]
# client_threads_list = [2]
ratio_list = [5]
percent_list = [50]
window_size_list = [10, 20, 30, 40, 50, 60]

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

# Generate all combinations of configuration variables
combinations = itertools.product(
    server_threads_list, client_threads_list, ratio_list, percent_list, window_size_list
)

for server_threads, client_threads, ratio, percent, window_size in combinations:
    dir_path = f"{dir_name}/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}"
    logdir = f"/tmp/masstree_logs_dlb/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}/"
    ckdir = f"/tmp/masstree_logs_dlb/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}/"
    server_command = f"sudo {exe_path}/dlb/mtd --logdir={logdir} --ckdir={ckdir} -j {server_threads} -k {client_threads}"
    snic_command = f"{exe_path}/dlb/rdma-dlb/rdma_dlb_snic -d {client_threads}"
    client_command = f"timeout --signal=SIGINT 600s {exe_path}/dlb/mtclient -s 192.168.200.10 -v mlx5_0 -g 3 -d {dir_path} -r {ratio} -p {percent} -w {window_size} -k {server_threads} -j {client_threads} -q {{qpn}} rw1"

    # Create directory for this combination if it doesn't exist
    if not os.path.exists(dir_path):
        os.makedirs(dir_path)
    
    # Establish SSH connections
    server_ssh = paramiko.SSHClient()
    server_ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    server_ssh.connect(server_hostname, username=server_username, password=user_password)

    snic_ssh = paramiko.SSHClient()
    snic_ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    snic_ssh.connect(snic_hostname, username=snic_username, password=user_password)
    
    # Make directory if not exist
    logdir_mkdir_command = f'rm -rf {logdir} && mkdir -p {logdir}'
    ckdir_mkdir_command = f'rm -rf {ckdir} && mkdir -p {ckdir}'
    server_ssh.exec_command(logdir_mkdir_command)
    server_ssh.exec_command(ckdir_mkdir_command)

    print(f"dlb Running -r {ratio} -p {percent} -w {window_size} -k {server_threads} -j {client_threads}")

    # Run server command and extract QPN
    stdin, stdout, stderr = server_ssh.exec_command(server_command, get_pty=True)
    # Write the sudo password to stdin (it must be followed by a newline)
    stdin.write(server_password + "\n")
    stdin.flush()
    print("Set up connection and run cmd on server...")
    time.sleep(10)  # Wait for server to start

    qpn = None
    while qpn is None:
        line = stdout.readline()
        if not line:
            break
        # print(line)
        qpn_match = re.search(r"\(int\)QPN (\d+)", line)
        if qpn_match:
            qpn = int(qpn_match.group(1))

    print(f"Extracted QPN: {qpn}")  # Debug print to check QPN extraction
    if qpn is None:
        print("Failed to extract QPN from server output")
        exit(1)
        

    # Run SNIC command
    snic_ssh.exec_command(snic_command)
    print("Set up connection with SNIC...")
    time.sleep(10)

    # Run client command with extracted QPN
    client_command = client_command.replace("{qpn}", str(qpn))
    with open(f"{dir_path}/log.txt", "w") as log_file:
        client_process = subprocess.Popen(client_command.split(), stdout=log_file)
    client_process.communicate()  # Wait for the client command to finish
    stdout.close()
    stdin.close()
    
    # Kill SNIC program after client process completes
    print("Killing SNIC process...")
    kill_snic_command = "pkill -f rdma_dlb_snic"
    snic_ssh.exec_command(kill_snic_command)
    
    # Clear Masstree logs
    # print("Clearing Masstree logs...")
    # clear_server_command = "sudo rm kvd-log-*"
    # server_ssh.exec_command(clear_server_command)

    # Close SSH connections
    server_ssh.close()
    snic_ssh.close()
    time.sleep(5)