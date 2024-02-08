#!/bin/bash

if [ -z $1 ]; then
    echo "Specify a process pattern to trace."
    exit 1
fi
procname="$1"

for pid in $(ps -ALf |grep "$procname" |awk '{print $4}'); do 
    output=$(gdb --q --n --ex bt --batch --pid $pid 2>&1)
    echo "=== $i ==="
    echo "$output"|grep -E "^#"
    echo ""
done
