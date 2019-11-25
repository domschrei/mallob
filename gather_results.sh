#!/bin/bash

logdir="$1"

cat $logdir/* | sed 's/^\[//g' | LC_ALL=C sort -s -g | awk '{print "["$0}'  > $logdir/log

jobs=`cat $logdir/log | grep -oE "#[0-9]+:0" | grep -oE "#[0-9]+"|sort -u|tr '\n' ' '`
echo $jobs
for job in $jobs; do
    cat $logdir/log | grep -E "${job}:|${job} |${job}," > $logdir/log$job
done

