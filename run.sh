#!/bin/bash

if [ -f TERMINATE_GLOBALLY_NOW ]; then
    rm TERMINATE_GLOBALLY_NOW
fi

cmd=""
if [ x"$1" == x"valgrind" ]; then
    cmd="valgrind"
    shift 1
fi

# Number of nodes to launch
if echo "$1" | grep -qE "^[0-9]+$" ; then 
    NP="$1"
    shift 1
else
    NP="9"
    echo "Defaulting to $NP nodes."
fi

# Resource scheduling / get tokens for mpi processes
#if command -v nonexclusive ; then
#    cmd="$cmd nonexclusive"
#fi

# Logging directory
logdir=logs/`date +%s`
mkdir -p $logdir

# Execute program
executable="build/mallob"
if [ "x$MPIRUN" == "x" ]; then
    MPIRUN="mpirun"
fi
echo $MPIRUN' -np "'$NP'" '$cmd' '$executable' '$@' -log='$logdir
$MPIRUN -np "$NP" $cmd $executable $@ -log=$logdir

# Post-execution: Gather logs
bash gather_results.sh "$logdir"
