#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: [HOSTFILE=<hostfile>] $0 [mallob-options]"
fi

hostfileopt=""
if [ -f $HOSTFILE ]; then
    hostfileopt="--hostfile $HOSTFILE"
    echo "Setting $hostfileopt"
fi

export MALLOC_CONF="thp:always"
export OMPI_MCA_btl_vader_single_copy_mechanism=none
export RDMAV_FORK_SAFE=1

command="mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root $hostfileopt --bind-to none \
-x MALLOC_CONF=thp:always -x OMPI_MCA_btl_vader_single_copy_mechanism=none -x RDMAV_FORK_SAFE=1 \
mallob $@"

echo "EXECUTING: $command"
$command
