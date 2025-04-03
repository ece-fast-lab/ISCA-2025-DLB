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

dir_name = f"{repo_path}/scripts/fig10/results_{reviewer_id}/dpdk-pd"

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

# Create results directory if it doesn't exist
if not os.path.exists(dir_name):
    os.makedirs(dir_name)

# Generate all combinations of configuration variables
combinations = itertools.product(
    server_threads_list, client_threads_list, ratio_list, percent_list, window_size_list
)

for server_threads, client_threads, ratio, percent, window_size in combinations:
    dir_path = f"{dir_name}/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}"
    logdir = f"/tmp/masstree_logs_dpdk_pd/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}/"
    ckdir = f"/tmp/masstree_logs_dpdk_pd/k{server_threads}_w{window_size}_j{client_threads}_r{ratio}_p{percent}/"
    server_command = f'sudo {exe_path}/dpdk-pd/mtd -l 0-31 -a "17:00.0" -- --logdir={logdir} --ckdir={ckdir} -j {server_threads}'
    client_command = f"timeout --signal=SIGINT 400s {exe_path}/dpdk-pd/mtclient -s 192.168.200.10 -v mlx5_0 -g 3 -d {dir_path} -r {ratio} -p {percent} -w {window_size} -k {server_threads} -j {client_threads} -q {{qpn}} rw1"

    # Create directory for this combination if it doesn't exist
    if not os.path.exists(dir_path):
        os.makedirs(dir_path)

    # Establish SSH connections
    server_ssh = paramiko.SSHClient()
    server_ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    server_ssh.connect(server_hostname, username=server_username, password=server_password)

    # Make directory if not exist
    logdir_mkdir_command = f'rm -rf {logdir} && mkdir -p {logdir}'
    ckdir_mkdir_command = f'rm -rf {ckdir} && mkdir -p {ckdir}'
    server_ssh.exec_command(logdir_mkdir_command)
    server_ssh.exec_command(ckdir_mkdir_command)

    print(f"dpdk-pd Running -r {ratio} -p {percent} -w {window_size} -k {server_threads} -j {client_threads}")

    # Run server command and extract QPN
    stdin, stdout, stderr = server_ssh.exec_command(server_command, get_pty=True)
    time.sleep(3)
    stdin.write(server_password + "\n")
    stdin.flush()
    time.sleep(10)  # Wait for server to start

    qpn = None
    while qpn is None:
        line = stdout.readline()
        # print(line.strip())  # Debug print to check server output
        if not line:
            break
        qpn_match = re.search(r"\(int\)QPN (\d+)", line)
        if qpn_match:
            qpn = int(qpn_match.group(1))

    print(f"Extracted QPN: {qpn}")  # Debug print to check QPN extraction
    if qpn is None:
        print("Failed to extract QPN from server output")
        exit(1)

    # Run client command with extracted QPN
    client_command = client_command.replace("{qpn}", str(qpn))
    max_retries = 2
    retry_count = 0
    while True:
        with open(f"{dir_path}/log.txt", "w") as log_file:
            # Capture both stdout and stderr
            command = "stdbuf -oL " + client_command
            client_process = subprocess.Popen(
                command.split(),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                bufsize=1
            )
            
            # Read and print output line by line in real time.
            for line in iter(client_process.stdout.readline, ''):
                print(line, end='', flush=True)  # Print to console immediately.
                log_file.write(line)               # Write the same output to the log file.
            
            client_process.stdout.close()
            client_process.wait()

        with open(f"{dir_path}/log.txt", "r") as log_file:
            log_contents = log_file.read()

        if "timeout" in log_contents.lower() or "total RPS" not in log_contents or len(log_contents) == 0:
            retry_count += 1
            if retry_count < max_retries:
                continue
        break
    stdout.close()
    stdin.close()

    kill_snic_command = "pkill -f mtd"
    server_ssh.exec_command(kill_snic_command)

    # Close SSH connections
    server_ssh.close()

    time.sleep(5)
