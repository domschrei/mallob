#!/bin/bash

if [ -z $1 ]; then
    echo "Provide a log directory."
    exit 1
fi

logdir="$1"
cd "$logdir"

# For each MPI process
for d in *; do
    if [ ! -d $d ]; then continue; fi
    echo $d
    if [ -f $d/$d.log ]; then 
        echo "$d/$d.log already exists."
        continue
    fi
    cat $d/* |sort -g -s > $d/$d.log
done
