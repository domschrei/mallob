#!/bin/bash

usage="Usage: [MALLOB_HOSTFILE=<hostfile>|MALLOB_NP=<num-procs>] $0 <input-cnf> [mallob-options]"
if [ -z "$1" ]; then
    echo $usage
    exit 1
fi

n_threads_per_process=$(nproc)
sharingspersec=2 # integer!!
nglobalprocs=$MALLOB_NP # via "-np argument of mpirun (on a single machine)"
if [ -z $nglobalprocs ]; then
    nglobalprocs=$(cat $MALLOB_HOSTFILE|wc -l) # via hostfile with one host per line
fi
if [ -z $nglobalprocs ]; then
    echo $usage
    exit 1
fi

echo "Running Mallob with $n_threads_per_process threads on $(hostname) as leader and with $nglobalprocs MPI processes in total"

options="-mono=$1 -pre-cleanup=1 `#-zero-only-logging` -v=3 -t=${n_threads_per_process} \
-processes-per-host=1 -regular-process-allocation=1 -sleep=1000 -trace-dir=/tmp"
shift 1
options="$options $@"

bash $(dirname "$0")/run-in-docker.sh $options
