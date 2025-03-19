#!/bin/bash

basedir="$1"

for d in $basedir/*/; do
    if [ ! -f $d/instance.txt ]; then continue; fi
    instance="$(basename $(cat $d/instance.txt))"

    id=$(grep -oE "to internal ID #[0-9]+" $d/0/log.0.i|head -1|grep -oE "[0-9]+")
    sol=$(cat $d/0/log.0|grep -oE "SOLUTION #$id [A-Z]+"|awk '{print $NF}')
    time=$(grep "RESPONSE_TIME #$id" $d/0/log.0|tail -1|awk '{print $6}')

    echo "$instance $sol $time"

done | awk 'NF==3' | sort -k 1,1b > $basedir/qtimes.txt

echo "$(cat $basedir/qtimes.txt | awk '$2 != "UNKNOWN"' | wc -l) solved"
echo "$(cat $basedir/qtimes.txt | awk '$2 == "SATISFIABLE"' | wc -l) satisfiable"
echo "$(cat $basedir/qtimes.txt | awk '$2 == "UNSATISFIABLE"' | wc -l) unsatisfiable"
