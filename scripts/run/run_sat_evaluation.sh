#!/bin/bash

set -e

if [ -z $2 ]; then
    cd instances
    > _benchmark_local
    for f in *.cnf ; do
       echo $f >> _benchmark_local
    done
    shuf _benchmark_local -o _benchmark_local
    cd ..
    benchmarkfile="instances/_benchmark_local"
else
    benchmarkfile=$2
fi

testcount=1
source $(dirname "$0")/systest_commons.sh

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,in,out}/
mkdir -p runs
cleanup

# Set options (1)
procs_per_job=2

# Generate independent jobs
n=0
i=1
while read -r instance; do
    # wallclock limit, arrival, dependencies, application
    wclimit=1000s application=SAT maxdemand=$procs_per_job introduce_job solve-$i instances/$instance
    n=$((n+1))
    i=$((i+1))
done < $benchmarkfile

# Set options (2)
num_procs=$1
num_active_workers=$(($num_procs-1))
num_parallel_jobs=$(($num_active_workers/$procs_per_job))
options="-c=1 -w=$num_active_workers -ajpc=$num_parallel_jobs -J=$n -t=3 -satsolver=kkkkkc -v=4 -pls=0 -mlpt=0"

# Launch Mallob
runid="sateval_$(hostname)_$(git rev-parse --short HEAD)_np${1}_"$(echo $options|sed 's/-//g'|sed 's/=//g'|sed 's/ /_/g')
RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun -np $1 --oversubscribe build/mallob -log=runs/$runid $options 2>&1 > OUT &

echo "Use \"tail -f OUT\" to follow output"
echo "Use \"killall mpirun\" to terminate"
sleep 1; echo "" # To mend ugly linebreak done by nohup
