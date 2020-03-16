
import re
import sys
import matplotlib
matplotlib.use('Qt5Agg') 
import matplotlib.pyplot as plt


def append(d, key, entry):
    if key not in d:
        d[key] = []
    d[key] += [entry]
    
def plot_xy(X, Y, color, linewidth, linestyle, markersize, markerstyle):
    plt.plot(X, Y, color=color, linestyle=linestyle, linewidth=linewidth, markersize=markersize, marker=markerstyle)

ranks = set()
time_offsets = dict()
min_time = -1
max_time = 9223372036854775807

balancing_starts = dict()
balancing_ends = dict()
communication_starts = dict()
communication_ends = dict()
load_one = dict()
load_zero = dict()
job_starts = dict()
job_ends = dict()
job_clients = dict()

messages = []
balance_messages = []
bounce_messages = []

colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']
linestyles = [":", "-.", "-", "--"]
linewidths = [3, 4, 3, 4, 4, 3, 4, 3]
job_colors = dict()
job_linestyles = dict()
job_linewidths = dict()

if len(sys.argv) > 2:
    max_time = float(sys.argv[-1])

# Collect data
for filename in sys.argv[1:-1]:
    time = 0
    for line in open(filename, "r").readlines():
        line = line.replace("\n", "")
        match = re.search(r'^([0-9]+\.[0-9]+) \[([0-9]+)\] (.*)$', line)
        if not match:
            continue
        time = float(match.group(1))
        rank = int(match.group(2))
        msg = match.group(3)
        
        ranks.add(rank)
        if min_time == -1:
            min_time = time
            max_time += time
        if time > max_time:
            break
        
        if "Passed global initialization barrier" in msg:
            #time_offsets[rank] = 0
            time_offsets[rank] = time
        if "Entering rebalancing" in msg:
            append(balancing_starts, rank, time-time_offsets[rank])
        if "Rebalancing completed" in msg:
            append(balancing_ends, rank, time-time_offsets[rank])
        if "Collecting clauses on this node" in msg:
            append(communication_starts, rank, time-time_offsets[rank])
        if "digested clauses" in msg:
            append(communication_ends, rank, time-time_offsets[rank])
        if "Processing message of tag" in msg:
            if "of tag 3" in msg:
                continue
            match = re.search(r'<= \[([0-9]+)\]$', msg)
            if match:
                messages += [[time-time_offsets[rank], int(match.group(1)), rank]]
        if ("Red. " in msg or "Brc. " in msg) and "Sending" in msg:
            match = re.search(r'=> \[([0-9]+)\]$', msg)
            if match:
                balance_messages += [[time-time_offsets[rank], rank, int(match.group(1))]]
        if "LOAD 1" in msg:
            match = re.search(r'\(\+\#([0-9]+):([0-9]+)\)', msg)
            if not match:
                print("No match in " + msg)
                exit(1)
            job_id = match.group(1)
            append(load_one, rank, [time-time_offsets[rank], job_id])
        if "LOAD 0" in msg:
            append(load_zero, rank, time-time_offsets[rank])
        if "Introducing job" in msg or "RESPONSE_TIME" in msg:
            match = re.search(r'\#([0-9]+)', msg)
            if not match:
                print("No match in " + msg)
                exit(1)
            job_id = match.group(1)
            if "Introducing job" in msg:
                job_starts[job_id] = time-time_offsets[rank]
                job_clients[job_id] = rank
                match = re.search(r' => \[([0-9]+)\]', msg)
                if not match:
                    print("No match in " + msg)
                    exit(1)
                bounce_messages += [[time-time_offsets[rank], rank, int(match.group(1))]]
            else:
                job_ends[job_id] = time-time_offsets[rank]
        if "Bouncing #" in msg:
            match = re.search(r'Bouncing \#([0-9]+):([0-9]+) => \[([0-9]+)\]', msg)
            if not match:
                print("No match in " + msg)
                exit(1)
            bounce_messages += [[time-time_offsets[rank], rank, int(match.group(3))]]

if max_time == 9223372036854775807:
    max_time = time

print(str(len(messages)) + " messages")
print(str(len(balance_messages)) + " balance messages")
print(str(len(bounce_messages)) + " bounce messages")

# Plot data

# dashed horizontal lines for each rank
for rank in ranks:
    plot_xy([0, max_time-min_time], [rank, rank], 'gray', 1, 'dashed', 0, 'x')

for rank in balancing_starts:
    plot_xy(balancing_starts[rank], [rank for x in balancing_starts[rank]], 'blue', 0, 'none', 5, 'x')
for rank in balancing_ends:
    plot_xy(balancing_ends[rank], [rank for x in balancing_ends[rank]], 'green', 0, 'none', 5, 'x')
for rank in communication_starts:
    plot_xy(communication_starts[rank], [rank for x in communication_starts[rank]], 'blue', 0, 'none', 5, '+')
for rank in communication_ends:
    plot_xy(communication_ends[rank], [rank for x in communication_ends[rank]], 'green', 0, 'none', 5, '+')
    
#for [time, sender, receiver] in messages:
#    plot_xy([time,time], [sender,receiver], 'gray', 1, '-', 0, 'x')

"""
for [time, sender, receiver] in bounce_messages:
    plt.annotate("", xy=(time, receiver), xytext=(time, sender), arrowprops=dict(arrowstyle="->", color="#999999"))
for [time, sender, receiver] in balance_messages:
    plt.annotate("", xy=(time, receiver), xytext=(time, sender), arrowprops=dict(arrowstyle="->"))

for [time, sender, receiver] in bounce_messages:
    plot_xy([time, time], [sender, receiver], "#999999", 1, '-', 0, 'x')
    if int(sender) > int(receiver):
        plot_xy([time], [receiver], "#999999", 0, '-', 5, 'v')
    else:
        plot_xy([time], [receiver], "#999999", 0, '-', 5, '^')
for [time, sender, receiver] in balance_messages:
    plot_xy([time, time], [sender, receiver], "#000000", 1, "-", 0, 'x')
    if int(sender) > int(receiver):
        plot_xy([time], [receiver], "#000000", 0, '-', 5, 'v')
    else:
        plot_xy([time], [receiver], "#000000", 0, '-', 5, '^')
"""

# Assign colors and shapes to occurring jobs
for job in job_starts:
    if job not in job_colors:
        idx = len(job_colors)
        job_colors[job] = colors[idx % len(colors)]
        job_linestyles[job] = linestyles[idx % len(linestyles)]
        job_linewidths[job] = linewidths[idx % len(linewidths)]

"""
for rank in load_one:
    for i in range(len(load_one[rank])):
        if rank in load_zero and i < len(load_zero[rank]):
            t2 = load_zero[rank][i]
        else:
            t2 = max_time-time_offsets[rank]
        [t, job] = load_one[rank][i]
        if job not in job_colors:
            idx = len(job_colors)
            job_colors[job] = colors[idx % len(colors)]
            job_linestyles[job] = linestyles[idx % len(linestyles)]
            job_linewidths[job] = linewidths[idx % len(linewidths)]
        
        plot_xy([t, t2], [rank, rank], job_colors[job], job_linewidths[job], job_linestyles[job], 0, 'x')
"""

for job_id in job_starts:
    rank = job_clients[job_id]
    plot_xy(job_starts[job_id], rank, job_colors[job_id], 0, "-", 10, "+")
for job_id in job_ends:
    rank = job_clients[job_id]
    plot_xy(job_ends[job_id], rank, job_colors[job_id], 0, "-", 10, "x")

# Show data
#plt.savefig("out.pdf")
plt.xlim(0, max_time-min_time+1)
plt.show()
