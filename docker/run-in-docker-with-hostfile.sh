#!/bin/bash

if [ -z "$2" ]; then
    echo "Usage: $0 <hostfile> <input-cnf> [mallob-options]"
    exit 1
fi

export MALLOC_CONF="thp:always"
export OMPI_MCA_btl_vader_single_copy_mechanism=none
export RDMAV_FORK_SAFE=1

n_threads_per_process=$(nproc)
sharingspersec=2 # integer!!
nglobalprocs=$(cat $1|wc -l)
echo "Running Mallob with $n_threads_per_process threads on $(hostname) as leader and with $nglobalprocs MPI processes in total"
bufferbasesize=$((375 * $n_threads_per_process / $sharingspersec))
echo "Buffer base size: $bufferbasesize"
portfolio=kkkccl
echo "Solver portfolio: $portfolio"

options="-mono=$2 -pre-cleanup=1 -seed=110519 `#-zero-only-logging` -v=3 -t=${n_threads_per_process} \
-max-lits-per-thread=50000000 -clause-buffer-base-size=$bufferbasesize -satsolver=$portfolio \
-processes-per-host=1 -regular-process-allocation=1 -sleep=1000 -trace-dir=/tmp"
shift 2
options="$options $@"

HOSTFILE="$1" bash run-in-docker-raw.sh $options
