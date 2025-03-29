import pandas as pd
import re
import matplotlib.pyplot as plt
import numpy as np
import os
from collections import defaultdict
from matplotlib.colors import Normalize, LinearSegmentedColormap
from matplotlib.cm import ScalarMappable
import matplotlib.patches as mpatches
from mpl_toolkits.axes_grid1 import make_axes_locatable
from math import log2
import math
import statistics


# Retrieve environment variables
repo_path = os.environ.get("REPO_PATH", "/home/user/ISCA-2025-DLB")
ssh_user = os.environ.get("HOST_ACCOUNT", "user")
ssh_host = os.environ.get("HOST_SSH_IP", "192.17.100.155")
reviewer_id = os.environ.get("REVIEWER_ID", "a")
user_password = os.environ.get("PASSWORD", "123456")

color_map = {
        'blue': '#4472C4',
        'orange': '#ED7D31',
        'yellow': '#FFC000',
        'green': '#70AD47',
    }

logfile = f"{repo_path}/scripts/fig13/results_{reviewer_id}.log"

# Regular expression patterns
run_pattern = re.compile(r'Running with NUM_EVENTS = \d+, NUM_TXS=(\d+), NUM_WORKERS=(\d+), NUM_FLOWS=(\d+), ENQUEUE_DEPTH=(\d+), CQ_DEPTH=(\d+)')
throughput_pattern = re.compile(r'Total worker throughput is (\d+\.\d+) Mpps')
latency_pattern = re.compile(r'Average worker latency is (\d+\.\d+) us')
worker_pattern = re.compile(r'Worker (\d) has (\d+) counts, (\d+\.\d+) us latency')
worker_tput_pattern = re.compile(r'Woker (\d) throughput is (\d+\.\d+) Mpps')

# # Data structure to hold results
results = defaultdict(lambda: defaultdict(list))
latencies = defaultdict(lambda: defaultdict(list))
worker_dist = defaultdict(lambda: defaultdict(list))
worker_tput = defaultdict(lambda: defaultdict(list))

def process_logfile_flows(filename):
    txs_ret = None
    workers_ret = None
    with open(filename, 'r') as file:
        num_txs = None
        num_workers = None
        num_flows = None
        for line in file:
            # Match the run pattern
            run_match = run_pattern.match(line)
            if run_match:
                num_txs = int(run_match.group(1))
                num_workers = int(run_match.group(2))
                num_flows = int(run_match.group(3))
                txs_ret = num_txs
                workers_ret = num_workers
                continue
            
            # Match the worker pattern
            worker_tput_match = worker_tput_pattern.search(line)
            if worker_tput_match:
                if num_txs is not None and num_workers is not None and num_flows is not None:
                    worker_tput[num_txs][num_workers][num_flows].append(float(worker_tput_match.group(2)))
            worker_match = worker_pattern.search(line)
            if worker_match:
                worker_counts = int(worker_match.group(2))
                if num_txs is not None and num_workers is not None and num_flows is not None:
                    worker_dist[num_txs][num_workers][num_flows].append(worker_counts)
            
            # Match the throughput pattern
            throughput_match = throughput_pattern.search(line)
            if throughput_match:
                throughput = float(throughput_match.group(1))
                if num_txs is not None and num_workers is not None and num_flows is not None:
                    results[num_txs][num_workers][num_flows].append(throughput)
            
            # Match the latency pattern
            latency_match = latency_pattern.search(line)
            if latency_match:
                latency = float(latency_match.group(1))
                if num_txs is not None and num_workers is not None and num_flows is not None:
                    latencies[num_txs][num_workers][num_flows].append(latency)
                    # Reset for the next entry
                    num_txs = None
                    num_workers = None
                    num_flows = None
    return txs_ret, workers_ret

def summarize_results_flows():
    summary = {}
    for num_txs in results:
        summary[num_txs] = {}
        for num_workers in results[num_txs]:
            summary[num_txs][num_workers] = {}
            for num_flows in results[num_txs][num_workers]:
                total_throughput = sum(results[num_txs][num_workers][num_flows])
                avg_latency = sum(latencies[num_txs][num_workers][num_flows])
                total_counts = sum(worker_dist[num_txs][num_workers][num_flows])
                worker_tputs = worker_tput[num_txs][num_workers][num_flows]
                worker_counts = worker_dist[num_txs][num_workers][num_flows]
                worker_percent = [c / total_counts * 100 for c in worker_counts]
                standard_deviation = statistics.stdev(worker_tputs)
                variance = statistics.variance(worker_tputs)
                cv_cnt = np.std(worker_counts) / np.mean(worker_counts)
                cv_tput = np.std(worker_tputs) / np.mean(worker_tputs)
                summary[num_txs][num_workers][num_flows] = {
                    "total_throughput": total_throughput,
                    "average_latency": avg_latency,
                    "total_counts": total_counts,
                    "worker_tputs": worker_tputs,
                    "worker_counts": worker_counts,
                    "worker_percent": worker_percent,
                    "std": standard_deviation,
                    "variance": variance,
                    "cv_cnt": cv_cnt,
                    "cv_tput": cv_tput
                }
    return summary


def get_summary_flows(summary):
    tputs = []
    lats = []
    stds = []
    cv_cnt = []
    cv_tput = []
    tputs_worker = []
    for num_txs in sorted(summary):
        for num_workers in sorted(summary[num_txs]):
            for num_flows in sorted(summary[num_txs][num_workers]):
                tputs.append(round(summary[num_txs][num_workers][num_flows]["total_throughput"], 4))
                lats.append(round(summary[num_txs][num_workers][num_flows]["average_latency"], 4))
                stds.append(round(summary[num_txs][num_workers][num_flows]["std"], 4))
                cv_cnt.append(round(summary[num_txs][num_workers][num_flows]["cv_cnt"], 4))
                cv_tput.append(round(summary[num_txs][num_workers][num_flows]["cv_tput"], 4))
                tputs_worker.append(summary[num_txs][num_workers][num_flows]["worker_tputs"])
    num_flows = sorted(summary[num_txs][num_workers])
    tputs_per_worker = [round(t / num_workers, 4) for t in tputs]
    # draw_plot_flows(num_flows, tputs_per_worker, lats, stds)
    draw_plot_box_flows(num_flows, tputs_worker, tputs, cv_tput)



def draw_plot_box_flows(num_flows, tputs_worker, tputs, cv_tput):
    # Convert num_flows to log2 scale
    log_num_flows = np.log2(num_flows)

    # Set up the figure
    plt.rcParams.update({'font.size': 16})
    fig, ax1 = plt.subplots(figsize=(14, 8))

    # Primary Y-Axis for tputs
    ax1.set_xscale('log', base=2)
    ax1.set_xticks(num_flows)
    ax1.set_xticklabels([f'2^{int(np.log2(x))}' for x in num_flows])
    ax1.set_xlim(8 / 2, 2048 * 2)  # Set limits to give some space around the data
    ax1.set_xlabel('Number of Flows (log2)')
    ax1.set_ylabel('Total Throughput (MPPS)', color='darkorange')
    line1, = ax1.plot(num_flows, tputs, marker='X', markersize=10, color='darkorange', label='Total Throughput', linewidth=3)
    ax1.tick_params(axis='y', labelcolor='darkorange')
    ax1.grid(which='both', linestyle='--', linewidth=0.7)

    # Create a secondary Y-Axis for tputs_worker
    ax2 = ax1.twinx()
    ax2.set_ylabel('Worker Throughput (MPPS)', color='royalblue')
    ax2.tick_params(axis='y', labelcolor='royalblue')

    # Define the visual width of each box on the plot
    visual_width = 0.1  # This is a fraction of the x-axis range

    # Plot the box plots for tputs_worker on the right Y-Axis
    for i, worker_data in enumerate(tputs_worker):
        # Calculate the position and width of each box in log space
        log_pos = log_num_flows[i]
        width = visual_width * (np.log2(ax1.get_xlim()[1]) - np.log2(ax1.get_xlim()[0])) * 0.5
        left = log_pos - width / 2
        right = log_pos + width / 2

        # Calculate quartiles and whiskers
        q1 = np.percentile(worker_data, 25)
        q3 = np.percentile(worker_data, 75)
        iqr = q3 - q1
        lower_whisker = max(np.min(worker_data), q1 - 1.5 * iqr)
        upper_whisker = min(np.max(worker_data), q3 + 1.5 * iqr)
        median = np.median(worker_data)
        outliers = [point for point in worker_data if point < lower_whisker or point > upper_whisker]

        # Draw the box
        ax2.fill_betweenx([q1, q3], 2**left, 2**right, color='lightblue', alpha=0.7, edgecolor='royalblue', lw=1.5)
        # Draw the whiskers
        ax2.plot([2**log_pos, 2**log_pos], [lower_whisker, q1], color='royalblue', lw=1.5)
        ax2.plot([2**log_pos, 2**log_pos], [q3, upper_whisker], color='royalblue', lw=1.5)
        # Draw the caps
        ax2.plot([2**left, 2**right], [lower_whisker, lower_whisker], color='royalblue', lw=1.5)
        ax2.plot([2**left, 2**right], [upper_whisker, upper_whisker], color='royalblue', lw=1.5)
        # Draw the median line
        ax2.plot([2**left, 2**right], [median, median], color='red', lw=2)

        # Plot outliers
        ax2.scatter([2**log_pos] * len(outliers), outliers, zorder=5, facecolors='none', edgecolor='royalblue')
        # Plot CV
        ax2.annotate(f'cv={round(cv_tput[i],3):.3f}', 
                     xy=(2**left, min(lower_whisker,min(outliers)) if len(outliers) else lower_whisker), 
                     xytext=(-5, -20), textcoords='offset points', fontsize=12, 
                     bbox=dict(boxstyle='round,pad=0.2', fc='gray', alpha=0.3))

    # Add legend and title
    line2 = plt.Line2D((0,1),(0,0), color='red', label='Medium per-Worker Throughput', lw=2)
    line3 = mpatches.Patch(color='gray', alpha=0.3, label='Coefficient of Variation')
    # line2 = mpatches.Patch(color='red', label='Avg Worker Throughput')
    ax1.legend(loc='upper left', handles=[line1, line2, line3], fontsize=12)
    ax1.set_title(f'Throughput for Different Number of Flows (Log2 Scale) with TX={num_txs}, WORKER={num_workers}')

    # Align yticks
    # ax1.set_yticks(np.linspace(0, math.ceil(ax1.get_ybound()[1]+1), 8))
    # ax2.set_yticks(np.linspace(0, math.ceil(ax2.get_ybound()[1]+1), 8))
    ax1.set_ylim(bottom=0)  # Ensure y-axis starts from 0
    ticks_ax1 = ax1.get_yticks()
    nticks = len(ticks_ax1) - 2
    lim1 = ax1.get_ylim()
    lim2 = (lim1[0], math.ceil(max([max(t) for t in tputs_worker])/nticks)*nticks)
    ax2.set_ylim(lim2)
    ticks_ax2 = ax2.get_yticks()
    # plt.locator_params(nbins=nticks+1)
    ax1.set_ylim(bottom=0, top=ticks_ax1[-2]*1.05)  # Ensure y-axis starts from 0
    ax2.set_ylim(bottom=0, top=ticks_ax2[-1]*1.05)  # Ensure y-axis starts from 0
    # ax2.set_yticks(np.linspace(0, ticks_ax2[-1], nticks+1))
    
    # Overlay ax1 over ax2
    ax1.set_zorder(ax2.get_zorder() + 1)
    ax1.patch.set_visible(False)
      
    # Show the plot
    plt.tight_layout()
    # ax1.xaxis.labelpad = 10
    # plt.savefig('Atomic_flows_tx_{}_worker_{}_box.png'.format(num_txs, num_workers))
    plt.savefig('fig13.png')
    # plt.show()


# Regular expression patterns
run_pattern = re.compile(r'Running with NUM_EVENTS = \d+, NUM_TXS=(\d+), NUM_WORKERS=(\d+), NUM_FLOWS=(\d+), ENQUEUE_DEPTH=(\d+), CQ_DEPTH=(\d+)')
throughput_pattern = re.compile(r'Total worker throughput is (\d+\.\d+) Mpps')
latency_pattern = re.compile(r'Average worker latency is (\d+\.\d+) us')


# Data structure to hold results
results = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
latencies = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
worker_dist = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
worker_tput = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
num_txs, num_workers = process_logfile_flows(logfile)
summary = summarize_results_flows()
get_summary_flows(summary)