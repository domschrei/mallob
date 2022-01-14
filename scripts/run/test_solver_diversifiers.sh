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


numconfigs=4

# For each config
for i in $(seq 0 $(($numconfigs-1))); do
    # For each formula
    export mallob_config=$i
    while read -r line; do
	echo $mallob_config $line
    done < "$benchmarkfile"
done > _combinations

# For each config-formula combination (shuffled)
shuf _combinations -o _combinations
instidx=1
cat _combinations|while read -r line; do    
	config=$(echo $line|awk '{print $1}')
	instance=$(echo $line|awk '{print $2}')
	echo $config $instidx $instance
	wclimit=1000s maxdemand=1 application=SAT appconfig='{"diversification-offset": "'$config'"}' introduce_job kissat-$(date +%s-%N)-$config-$instidx instances/$instance
	instidx=$((instidx+1))
done

RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun -np $(($1+1)) --map-by numa:PE=1 build/mallob \
-t=1 -c=1 -w=$1 -J=$(( $numconfigs * $(cat "$benchmarkfile"|wc -l) )) -satsolver=k -aold -pls=0 -ajpc=$1 -s=0 -checksums=1 \
-checkjsonresults 2>&1 > _systest &

sleep 2

