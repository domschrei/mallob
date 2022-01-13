 
import sys

num_nodes_per_job = dict()
#nodes_per_job = dict()
jobfiles = dict()
total_nodes = 0
#volumefile = open(sys.argv[1] + "/load_history", "w")

last_time_per_job = dict()
cpu_per_job = dict()

id_map = dict()
for line in open(sys.argv[1] + "/local_to_global_id", "r").readlines():
    line = line.rstrip()
    words = line.split(" ")
    id_map[int(words[0])] = int(words[1])

for line in open(sys.argv[1] + "/loadevents", "r").readlines():
    
    line = line.rstrip()
    words = line.split(" ")
    #print(line)
    
    time = float(words[0])
    rank = int(words[1])
    load = int(words[2])
    jobid = id_map[int(words[3])]
    index = int(words[4])
    
    if jobid not in num_nodes_per_job:
        #nodes_per_job[jobid] = set()
        cpu_per_job[jobid] = 0
        last_time_per_job[jobid] = time
        num_nodes_per_job[jobid] = 0
        #jobfiles[jobid] = open(sys.argv[1] + "/volume_history_#" + str(jobid), "w")
    
    if time - last_time_per_job[jobid] > 0:
        cpu_per_job[jobid] += (time - last_time_per_job[jobid]) * 5 * num_nodes_per_job[jobid]
        last_time_per_job[jobid] = time
    
    if load == 1:
        #nodes_per_job[jobid].add(rank)
        num_nodes_per_job[jobid] += 1
        total_nodes += 1
    else:
        #nodes_per_job[jobid].remove(rank)
        num_nodes_per_job[jobid] -= 1
        total_nodes -= 1
    
    #jobfiles[jobid].write(str(time) + " " + str(len(nodes_per_job[jobid])) + "\n")
    #volumefile.write(str(time) + " " + str(total_nodes) + "\n")

for jobid in cpu_per_job:
    print(jobid,":",cpu_per_job[jobid])

#for jobid in jobfiles:
#    jobfiles[jobid].close()
#volumefile.close()
