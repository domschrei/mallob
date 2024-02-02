#!/bin/bash

procname="trusted_checker_process"
#procname="mallob"
#procname="mallob_sat_process"

for i in $(ps -ALf |grep $procname |awk '{print $4}'); do 
    output=$(gdb --q --n --ex bt --batch --pid $i 2>&1)
#    if echo "$output"|grep -q "doWorkerNodeProgram"; then
        #if ! echo "$output"|grep -q "__GI___clock_nanosleep"; then
            echo "=== $i ==="
            echo "$output"|grep -E "^#"
        #fi
#    fi
done
