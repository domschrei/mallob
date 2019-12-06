#!/bin/bash

logdir="$1"
if [ "x$1" == "x" ]; then
    echo "No log dir provided"
    exit 1
fi

# Create a single log file, and a reduced one without mem conspt info
> $logdir/tmp
for f in $logdir/log_*.*; do
    echo $f
    while read -r line; do
        if echo $line|grep -q "vm_usage"; then
            echo $line >> $logdir/log_mem 
        else
            echo $line|sed 's/^\[//g' >> $logdir/tmp
    done < $f
done
LC_ALL=C sort -s -g $logdir/tmp | awk '{print "["$0}' > $logdir/log
rm $logdir/tmp

# Go through reduced log file and write jobs info into separate files
rm $logdir/log#* 2> /dev/null
while read -r line; do
    if echo $line|grep -qE "#[0-9]+"; then
        job=`echo $line|grep -oE "#[0-9]+"`
        if echo $line|grep -qE "${job}:|${job} |${job},|${job}."; then
            echo $line >> $logdir/log$job
        fi
    fi
done < $logdir/log
