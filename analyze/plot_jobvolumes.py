
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

min_time = -1
max_time = 9223372036854775807

job_times = dict()
job_volumes = dict()

# Aggregated job volumes
all_jobs_id = "$\Sigma$"
job_times[all_jobs_id] = [0]
job_volumes[all_jobs_id] = [0]

colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']
linestyles = ["-.", ":", "--", "-"]
linewidths = [2, 2, 2, 2, 2, 2, 2, 2]
job_colors = dict()
job_linestyles = dict()
job_linewidths = dict()
epsilon = 0.00000001


if len(sys.argv) > 2:
    max_time = float(sys.argv[2])

# Collect data
time = 0
for line in open(sys.argv[1], "r").readlines():
    line = line.replace("\n", "")
    
    match = re.search(r'^([0-9]+\.[0-9]+) \[([0-9]+)\] (.*)$', line)
    if match:
        time = float(match.group(1))
        if min_time == -1:
            min_time = time
        if time > max_time:
            break
    
    match = re.search(r'LOAD ([01]) \([+-]\#([0-9]+):([0-9]+)\)', line)
    if match:
        newload = int(match.group(1))
        job_id = match.group(2)
        
        if job_id not in job_times:
            job_times[job_id] = [time]
            job_volumes[job_id] = [0]
        
        for i in [job_id, all_jobs_id]:
        
            last_load = job_volumes[i][-1]
            append(job_times, i, time-epsilon)
            append(job_volumes, i, last_load)
            
            if newload == 1:
                append(job_times, i, time)
                append(job_volumes, i, last_load+1)
                print(str(i) + " +1")
            else:
                append(job_times, i, time)
                append(job_volumes, i, last_load-1)
                print(str(i) + " -1")

if max_time == 9223372036854775807:
    max_time = time


# Plot data
plt.figure(figsize=(4.5,3.5))

# Assign colors and shapes to occurring jobs
for job in job_times:
    if len(job_times[job]) > 1:
        if job not in job_colors:
            idx = len(job_colors)
            job_colors[job] = colors[idx % len(colors)]
            job_linestyles[job] = linestyles[idx % len(linestyles)]
            job_linewidths[job] = linewidths[idx % len(linewidths)]

# Plot volume graph for aggregation of all jobs
#times = job_times[all_jobs_id]
#volumes = job_volumes[all_jobs_id]
#plot_xy(times, volumes, all_jobs_id, "black", 1, "--", 0, "+")

# Plot volume graph for each job
for job_id in job_colors:
    if job_id == all_jobs_id:
        continue
    times = job_times[job_id]
    volumes = job_volumes[job_id]
    print(times)
    print(volumes)
    plot_xy(times, volumes, "\#" + str(job_id), job_colors[job_id], job_linewidths[job_id], job_linestyles[job_id], 0, "+")

# Show data
plt.legend()
plt.title("\\textit{mallob}: Volumes of concurrent jobs over time")
plt.xlabel("Elapsed time / s")
plt.xlim(min_time, max_time)
plt.ylabel("\# active nodes")
plt.ylim(0, None)
plt.tight_layout()
plt.show()
#plt.savefig("out.pdf")
