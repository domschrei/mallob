#!/bin/bash

set -e

# Parameters to supply:
# 1st parameter: number of MPI processes to start
# 2nd parameter (optional): a text file containing one CNF problem file path per line

# 2nd parameter missing?
if [ -z $2 ]; then
    # -- collect all CNF problems in the instances/ directory, write a text file
    cd instances
    > _benchmark_local
    for f in *.cnf ; do
       echo $f >> _benchmark_local
    done
    shuf _benchmark_local -o _benchmark_local # shuffle lines randomly
    cd ..
    benchmarkfile="instances/_benchmark_local"
else
    # use the user-supplied benchmark file
    benchmarkfile=$2
fi

# Import important functions (see scripts/systest_commons.sh)
testcount=1
source $(dirname "$0")/systest_commons.sh

# Directory setup
mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup

# Generate chain of independent jobs
i=1
while read -r instance; do
    # Parameters: job name, instance path, wallclock limit, arrival, dependencies, application (SAT or DUMMY)
    introduce_job solve-$i instances/$instance 300 0 "" SAT
    i=$((i+1))
done < $benchmarkfile

# Set options
options="-t=4 -lbc=4 -g=0.1 -satsolver=lcg -v=4 -J=$(($i-1)) -ch=1 -chaf=5 -chstms=60 -s=0.25 -cfhl=0 -smcl=8 -hmcl=8 -mlbdps=8 -checksums=0 -huca=0 -wam=1000 -sleep=100"

# Launch Mallob with a unique run ID for a logging directory
runid="sched_$(hostname)_$(git rev-parse --short HEAD)_np${1}_"$(echo $options|sed 's/-//g'|sed 's/=//g'|sed 's/ /_/g')
RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun -np $1 --oversubscribe build/mallob -log=runs/$runid $options 2>&1 > OUT &

echo "Use \"tail -f OUT\" to follow output"
echo "Use \"killall mpirun\" to terminate"
sleep 1; echo "" # To mend ugly linebreak done by nohup
