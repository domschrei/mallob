
import sys

cputimes = dict()
nodes = dict()
maxnodes = dict()
lasteventtime = dict()
jobbegintime = dict()
requesthops = dict()
ctxswitches = []

for i in range(1, 2001):
    cputimes[i] = 0
    nodes[i] = set()
    maxnodes[i] = 0
    lasteventtime[i] = 0
for i in range(90000, 91000):
    cputimes[i] = 0
    nodes[i] = set()
    maxnodes[i] = 0
    lasteventtime[i] = 0

# INPUT: loadevents file: <time> <rank> <newload> <jobid> <jobindex>
load_lines = open(sys.argv[1], 'r').readlines()
hops_lines = open(sys.argv[2], 'r').readlines()
l = 0
hl = 0
while l < len(load_lines):

    # Collect all events of the current time (may be multiple lines)

    line = load_lines[l].rstrip()
    words = line.split(" ")
    
    time = float(words[0])
    
    rank = int(words[1])
    load = int(words[2])
    jobid = int(words[3])
    jobidx = int(words[4])
    
    while rank >= len(ctxswitches):
        ctxswitches += [0]
    ctxswitches[rank] += 1
    
    events = dict()
    events[rank] = [[load,jobid,jobidx]]
    
    while l+1 < len(load_lines) and load_lines[l+1].startswith(words[0] + " "):
        l += 1
        line = load_lines[l].rstrip()
        words = line.split(" ")
        
        rank = int(words[1])
        load = int(words[2])
        jobid = int(words[3])
        jobidx = int(words[4])
        
        while rank >= len(ctxswitches):
            ctxswitches += [0]
        ctxswitches[rank] += 1
        
        if rank in events and [1-load,jobid,jobidx] in events[rank]:
            # Cancel out LOAD-0 and LOAD-1 events of same job on same node
            events.pop(rank, None)
        else:
            if rank not in events:
                events[rank] = []
            events[rank] += [[load,jobid,jobidx]]
    
    # Read all "num hops" until that point
    while hl < len(hops_lines):
        line = hops_lines[hl].rstrip()
        words = line.split(" ")
        
        htime = float(words[0])
        if htime > time:
            break
        
        rank = int(words[1])
        jobid = int(words[2])
        jobidx = int(words[3])
        hops = int(words[4])
        
        requesthops[(rank,jobid,jobidx)] = hops
        
        hl += 1
    
    # Process each event
    
    for rank in events:
        for load,jobid,jobidx in events[rank]:
    
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
                maxnodes[jobid] = max(maxnodes[jobid], len(nodes[jobid]))
                if (rank,jobid,jobidx) in requesthops:
                    print("HOPS",str(time),jobid,jobidx,requesthops[(rank,jobid,jobidx)])
                    del requesthops[(rank,jobid,jobidx)]
            else:
                if rank not in nodes[jobid]:
                    print("WARN: " + line + " : not computing!")
                    # Add max. time this node could already be computing on the job
                    cputimes[jobid] += (time - jobbegintime[jobid])
                nodes[jobid].discard(rank)
            
            print("NODES",str(time),len(nodes[jobid]),jobid)
    
    l += 1

for i in range(1, 401):
    print("CPUTIME",i, 4*cputimes[i]/3600) # 4 CPUs per rank

for i in range(1, 401):
    print("MAXNODES",i,maxnodes[i])
    
for i in range(len(ctxswitches)):
    print("CTXSWITCHES",i,ctxswitches[i])
