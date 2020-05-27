
import sys

cputimes = dict()
nodes = dict()
lasteventtime = dict()
jobbegintime = dict()
for i in range(1, 401):
    cputimes[i] = 0
    nodes[i] = set()
    lasteventtime[i] = 0

lines = open(sys.argv[1], 'r').readlines()
l = 0
while l < len(lines):

    # Collect all events of the current time (may be multiple lines)

    line = lines[l].rstrip()
    words = line.split(" ")
    time = float(words[0])
    events = dict()
    
    rank = int(words[1])
    load = int(words[2])
    jobid = int(words[3])
    events[rank] = [[load,jobid]]
    
    while l+1 < len(lines) and lines[l+1].startswith(words[0]):
        l += 1
        line = lines[l].rstrip()
        words = line.split(" ")
        rank = int(words[1])
        load = int(words[2])
        jobid = int(words[3])
        if rank in events and [1-load,jobid] in events[rank]:
            # Cancel out LOAD-0 and LOAD-1 events of same job on same node
            events.pop(rank, None)
        else:
            if rank not in events:
                events[rank] = []
            events[rank] += [[load,jobid]]
    
    # Process each event
    
    for rank in events:
        for load,jobid in events[rank]:
    
            if jobid not in jobbegintime:
                jobbegintime[jobid] = time
        
            # Add up CPU time from last time slice
            period = time - lasteventtime[jobid]
            cputimes[jobid] += period * len(nodes[jobid])
            lasteventtime[jobid] = time
            
            # Change set of involved nodes
            if load == 1:
                if rank in nodes[jobid]:
                    print("WARN: " + line + " : already computing!")
                nodes[jobid].add(rank)
            else:
                if rank not in nodes[jobid]:
                    print("WARN: " + line + " : not computing!")
                    # Add max. time this node could already be computing on the job
                    cputimes[jobid] += (time - jobbegintime[jobid])
                nodes[jobid].discard(rank)
            
            print("NODES",str(time),len(nodes[jobid]),jobid)
    
    l += 1

for i in range(1, 401):
    print(i, 4*cputimes[i]/3600) # 4 CPUs per rank