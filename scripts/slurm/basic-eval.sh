#!/bin/bash

basedir="$1"

for d in $basedir/*/; do
    if [ ! -f $d/instance.txt ]; then continue; fi
    if ! grep -qE "SOLUTION #.*SATISFIABLE" $d/0/log.0 ; then continue; fi
    instance="$(basename $(cat $d/instance.txt))"

    sol=$(cat $d/0/log.0|grep -oE "SOLUTION #[0-9]+ (UN)?SATISFIABLE"|tail -1|awk '{print $NF}')
    time=$(grep -oE "0 RESPONSE_TIME #[0-9]+ [0-9\.]+ rev" $d/0/log.0|awk '{print $4}'|sort -g|tail -1)

    echo "$instance $sol $time"

done | awk 'NF==3' | sort -k 1,1b > $basedir/qtimes.txt

echo "$(cat $basedir/qtimes.txt | awk '$2 != "UNKNOWN"' | wc -l) solved"
echo "$(cat $basedir/qtimes.txt | awk '$2 == "SATISFIABLE"' | wc -l) satisfiable"
echo "$(cat $basedir/qtimes.txt | awk '$2 == "UNSATISFIABLE"' | wc -l) unsatisfiable"
