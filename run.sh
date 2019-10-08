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
mkdir -p logs

# Execute program
echo 'mpirun -np "'$NP'" '$cmd' ./mallob '$@' | tee logs/log.'`date +%s`
mpirun -np "$NP" $cmd ./mallob $@ | tee logs/log.`date +%s`
