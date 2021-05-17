#!/bin/bash

export LC_ALL=C.UTF-8

 # Pick subset of instances
cat scenarios/instancelist | sort -R | head -40 > instances

# Add blocks of repeated instances with different priorities
>priorized_instances
for rep in {1..10}; do
    while read -r line; do
    
        for prio in $(seq 0.1 0.1 1.0); do
            echo $prio $line
        done
        
    done < instances | shuf >> priorized_instances
done

# Number of effective CPUs 
busycpus="1516" #3884
# CPUsec timeout per job
maxcpusecs=$((10*3600))
totalnumjobs=$(cat priorized_instances|wc -l)

# Calculate lambda
totalduration=$(echo "$totalnumjobs * $maxcpusecs / $busycpus"|bc -l)
lambda=$(echo "$totalduration / $totalnumjobs"|bc -l)
echo "# Total_duration: $totalduration   lambda: $lambda"

# Sample from exponential distribution
python3 -c "import numpy as np
a = np.random.exponential($lambda, $totalnumjobs)
t = 0.0
c = 1
for x in a:
    t += x
    print('%i %.3f' % (c,t))
    c += 1" > ids_arrivals

# Paste everything together
echo "# ID Arv Prio File"
paste -d " " ids_arrivals priorized_instances
    
# Clean up
rm instances priorized_instances ids_arrivals
