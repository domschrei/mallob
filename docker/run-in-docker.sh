#!/bin/bash

usage="Usage: [MALLOB_HOSTFILE=<hostfile>|MALLOB_NP=<num-procs>] $0 [mallob-options]"
if [ -z "$1" ]; then
    echo $usage
    exit 1
fi

mpiopt=""
if [ -f "$MALLOB_HOSTFILE" ]; then
    mpiopt="--hostfile $MALLOB_HOSTFILE"
elif [ ! -z "$MALLOB_NP" ]; then
    mpiopt="-np $MALLOB_NP"
else
    echo $usage
    exit 1
fi
echo "mpiopt: $mpiopt"

export MALLOC_CONF="thp:always"
export OMPI_MCA_btl_vader_single_copy_mechanism=none
export RDMAV_FORK_SAFE=1

command="mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root $mpiopt --bind-to none \
-x MALLOC_CONF=thp:always -x OMPI_MCA_btl_vader_single_copy_mechanism=none -x RDMAV_FORK_SAFE=1 \
build/mallob $@"

echo "EXECUTING: $command"
$command
