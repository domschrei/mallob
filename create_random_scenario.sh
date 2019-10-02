#!/bin/bash

function rand() {
    
    awk -v n=1 -v seed="$RANDOM" 'BEGIN { srand(seed); for (i=0; i<n; ++i) printf("%.4f\n", rand()) }'
    
}

numjobs="$1"

shuf instance_list | head -$numjobs > selection

id=1
arrival=1
echo "# ID Arv Prio File"
while read -r filename; do

    r=`rand`
    arrival=`echo $arrival + 10*$r|bc`
    echo "$id $arrival 1 instances/$filename"
    id=$((id+1))

done < selection

rm selection
