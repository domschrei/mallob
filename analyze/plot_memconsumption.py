
import re
import sys
import matplotlib
matplotlib.use('Qt5Agg') 
import matplotlib.pyplot as plt

def append(d, key, entry):
    if key not in d:
        d[key] = []
    d[key] += [entry]
    
def plot_xy(X, Y, label, color, linewidth, linestyle, markersize, markerstyle):
    plt.plot(X, Y, label=label, color=color, linestyle=linestyle, linewidth=linewidth, markersize=markersize, marker=markerstyle)

ranks = set()
min_time = -1
max_time = 9223372036854775807

vm_per_rank = dict()
rss_per_rank = dict()
times_per_rank = dict()

colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']
markers = ['^', 's', 'o', '+', 'x', 's', '^', '*', 'o']
linestyles = ["-.", ":", "--", "-"]
linewidths = [2, 2, 2, 2, 2, 2, 2, 2]
rank_colors = dict()
rank_markers = dict()
rank_linestyles = dict()
rank_linewidths = dict()
epsilon = 0.00000001


if len(sys.argv) > 2:
    max_time = float(sys.argv[2])

# Collect data
for line in open(sys.argv[1], "r").readlines():
    line = line.replace("\n", "")
    match = re.search(r'^([0-9]+\.[0-9]+) \[([0-9]+)\] (.*)$', line)
    if not match:
        continue
    reltime = float(match.group(1))
    rank = int(match.group(2))
    msg = match.group(3)
    
    if rank not in ranks:
        ranks.add(rank)
        times_per_rank[rank] = [0]
        vm_per_rank[rank] = [0]
        rss_per_rank[rank] = [0]
        
    if reltime > max_time:
        break
    
    if "rss=" in msg:
        match = re.search(r'vm=([0-9\.]+).*rss=([0-9\.]+)', msg)
        if not match:
            print("No match in " + msg)
            continue
        vm = float(match.group(1))
        rss = float(match.group(2))
        times_per_rank[rank] += [reltime]
        vm_per_rank[rank] += [vm]
        rss_per_rank[rank] += [rss]

if max_time == 9223372036854775807:
    max_time = reltime



# Plot data
plt.figure(figsize=(4.5,3.5))

# Assign colors and shapes to occurring jobs
for rank in ranks:
    if rank not in rank_colors:
        idx = len(rank_colors)
        rank_colors[rank] = colors[idx % len(colors)]
        rank_linestyles[rank] = linestyles[idx % len(linestyles)]
        rank_linewidths[rank] = linewidths[idx % len(linewidths)]
        rank_markers[rank] = markers[idx % len(markers)]

# Plot volume graph for each job
for rank in ranks:
    times = times_per_rank[rank]
    vm = vm_per_rank[rank]
    rss = rss_per_rank[rank]
    plot_xy(times, rss, str(rank), rank_colors[rank], rank_linewidths[rank], rank_linestyles[rank], 5, rank_markers[rank])

# Show data
plt.legend()
plt.title("\\textit{mallob}: Memory consumption of each rank")
plt.xlabel("Elapsed time / s")
#plt.xlim(min_time, max_time)
plt.ylabel("Memory consumption (GB)")
plt.ylim(0, None)
plt.tight_layout()
plt.show()
#plt.savefig("out.pdf")
 
