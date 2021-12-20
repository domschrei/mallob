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
mkdir -p .api/jobs.0/{in,out}/
cleanup


numconfigs=15

# For each config
for i in $(seq 0 $(($numconfigs-1))); do

    # For each formula (shuffled order)
    shuf "$benchmarkfile" -o _shuf
    while read -r line; do

	    wclimit=1000 maxdemand=1 application=SAT appconfig='{"diversification-offset": "'$i'"}' introduce_job cadical-$i-job-$(date +%s-%N) instances/$line

    done < _shuf

done

RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun -np $(($1+1)) --map-by numa:PE=1 build/mallob \
-t=1 -c=1 -w=$1 -J=$(( $numconfigs * $(cat "$benchmarkfile"|wc -l) )) -satsolver=c -pls=0 -lbc=$1 -s=0 -checksums=1 \
-checkjsonresults 2>&1 > _systest &
