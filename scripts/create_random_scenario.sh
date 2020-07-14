#!/bin/bash

function rand_arrival() {
    awk -v seed="$RANDOM" 'BEGIN { srand(seed); printf("%.4f\n", '$2'*rand()+'$1') }'
}
function rand_priority() {
    awk -v seed="$RANDOM" 'BEGIN { srand(seed); printf("%.4f\n", 0.998*rand()+0.001) }'
}

numjobs="$1"
shift 1
if [ "x$1" == "x" ]; then
    numclients="1"
else
    numclients="$1"
    shift 1
fi
if [ "x$1" == "x" ]; then
    output="SCENARIO"
else
    output="$1"
    shift 1
fi
if [ "x$1" != "x" ]; then
    for i in {1..1000}; do
        echo $(($1+$i))" " >> randseed
    done
    shift 1
else
    for i in {1..1000}; do
        echo "0 " >> randseed
    done
fi

echo "$numjobs jobs, $numclients clients, output to $output"

id=1
ls instances/*.cnf | shuf --random-source randseed | head -$numjobs > selection
for client in `seq 0 $(($numclients-1))`; do
    echo "# ID Arv Prio File" > $output.$client
done

arrival=0
client=0
while read -r filename; do

    arrival=`rand_arrival $arrival 10`
    #echo "$id $arrival `rand_priority` $filename" >> $output.$client
    echo "$id $arrival 1 $filename" >> $output.$client
    id=$((id+1))
    client=$(($client+1))
    if [ $client == $numclients ]; then
        client=0
    fi

done < selection

rm selection
rm randseed

#arrival=0
#for i in {1..1000}; do
#    arrival=`rand_arrival $arrival 2`
#    echo $(($id+100000))" $arrival "`rand_priority`" /global_data/schreiber/sat_instances/#ridiculouslysimple_sat_1.cnf"
#    id=$((id+1))
#done
