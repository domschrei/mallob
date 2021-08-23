#!/bin/bash

set -e

if [ -z $2 ]; then
    cd instances
    > _benchmark_local
    for f in *.cnf ; do
       echo $f >> _benchmark_local
    done
    cd ..
    benchmarkfile="instances/_benchmark_local"
else
    benchmarkfile=$2
fi

testcount=1
source $(dirname "$0")/systest_commons.sh

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup

# Generate chain of interdependent jobs
n=0
prevjob=""
i=1
while read -r instance; do
    # wallclock limit, arrival, dependencies, application
    prevjob=$(introduce_job solve-$i instances/$instance 300 0 $prevjob SAT) 
    n=$((n+1))
done < $benchmarkfile

RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $1 --oversubscribe build/mallob \
-t=4 -l=1 -g=0.1 -cg=1 -satsolver=l -v=4 -T=600 -ch=1 -chaf=5 -chstms=60 -appmode=fork \
-cfhl=1 -smcl=30 -hmcl=30 -mlbdps=8 -checksums=0 -log=test_$$ -huca=0 -wam=10 -sleep=0
