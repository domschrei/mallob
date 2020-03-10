
import sys

runtime_files = sys.argv[1:]
runtime_lines = []
for f in runtime_files:
    runtime_lines += [open(f, 'r').readlines()]

num_competitors = len(runtime_lines)

indices = [0 for x in range(num_competitors)]

runtimes = []
speedups = [[] for x in range(num_competitors)]
total_runtimes = [0 for x in range(num_competitors)]
total_seq_runtimes = [0 for x in range(num_competitors)]


job_id = 1
while True:
    
    runtime = [-1 for x in range(num_competitors)]
    
    for i in range(num_competitors):
        if indices[i] >= len(runtime_lines[i]):
            continue
        if runtime_lines[i][indices[i]].startswith(str(job_id)):
            runtime[i] = float(runtime_lines[i][indices[i]].split(" ")[2])
            indices[i] += 1
    
    print(str(job_id) + " : " + str(runtime))
    runtimes += [runtime]
    
    for i in range(1, num_competitors):
        if runtime[0] >= 0 and runtime[i] >= 0:
            speedups[i] += [runtime[0] / runtime[i]]
            total_runtimes[i] += runtime[i]
            total_seq_runtimes[i] += runtime[0]
    
    job_id += 1
    if job_id > 400:
        break

for i in range(1, num_competitors):
    speedup = speedups[i]
    if not speedup:
        continue
    speedup.sort()
    mean_speedup = sum(speedup) / len(speedup)
    median_speedup = speedup[int(len(speedup)/2)]
    total_speedup = float(total_seq_runtimes[i]) / total_runtimes[i]
    
    print(runtime_files[i] + " : mean speedup=" + str(mean_speedup) + ", median speedup=" + str(median_speedup) + ", total speedup=" + str(total_speedup))
    
    with open(runtime_files[i] + "_speedups", 'w') as f:
        for s in speedup:
            f.write(str(s) + "\n")
