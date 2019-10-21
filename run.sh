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
echo 'mpirun -np "'$NP'" '$cmd' '$executable' '$@' -log='$logdir
mpirun -np "$NP" $cmd $executable $@ -log=$logdir

# Post-execution: Gather logs
cat $logdir/* | sed 's/^\[//g' | LC_ALL=C sort -g | awk '{print "["$0}'  > $logdir/log

jobs=`cat $logdir/log | grep -oE "#[0-9]+:0" | grep -oE "#[0-9]+"|sort -u|tr '\n' ' '`
echo $jobs
for job in $jobs; do
    cat $logdir/log | grep -E "${job}:|${job} |${job}," > $logdir/log$job
done
