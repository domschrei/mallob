#!/bin/bash

logdir="$1"
if [ "x$1" == "x" ]; then
    echo "No log dir provided"
    exit 1
fi

# Create a single log file, and a reduced one without mem conspt info
cat $logdir/log_*.*|grep -v "vm_usage" > $logdir/tmp
cat $logdir/tmp | LC_ALL=C sort -s -g | awk '{print $0}' > $logdir/log
rm $logdir/tmp

jobs=`grep -E "Introducing.*#[0-9]+" $logdir/log|grep -oE "#[0-9]+"|tr '\n' ' '`
echo $jobs

# Go through reduced log file and write jobs info into separate files
rm $logdir/log#* 2> /dev/null
for job in $jobs; do
    grep -E "${job}:|${job} |${job},|${job}\." $logdir/log > $logdir/log$job
done
