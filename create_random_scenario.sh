#!/bin/bash

function rand() {
    awk -v n=1 -v seed="$RANDOM" 'BEGIN { srand(seed); for (i=0; i<n; ++i) printf("%.4f\n", 10*rand()+'$1') }'
}

numjobs="$1"

ls instances/*.cnf | shuf | head -$numjobs > selection

id=1
arrival=0
echo "# ID Arv Prio File"
while read -r filename; do

    arrival=`rand $arrival`
    echo "$id $arrival 1 $filename"
    id=$((id+1))

done < selection

rm selection
