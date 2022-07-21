#!/bin/bash
pcName="i10pc135"
folder="latestResults" #latestResults
instanceName="covtypeShuffle" #   mnist784      benign_traffic      covtypeShuffle
instanceFirstLine="54 55 550000" #7 7 209    115 115 52150      54 55 581012
kList=(10 20 30 40 50) # 60 70 80
npList=(160 155 150 145 140 135 130 128 127 125 120 115 110 105 100 95 90 85 80 75 70 65 64 63 60 55 50 45 40 35 32 31 30 25 20 16 15 10 8 7 5 4 3 2 1) 
countPasses=3
#k=20
for k in ${kList[@]}; do


    echo "$k $instanceFirstLine" > ./instances/${instanceName}${k}.csv
    cat ./instances/${instanceName}K.csv >> ./instances/${instanceName}${k}.csv
    > ./Testing/${folder}/times-${pcName}-${k}.txt
    for n in ${npList[@]}; do #((n = 158 ; n < 161 ; ++n));
        # v=3 
        #PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3
        # v=0 >> ./Testing/times-${k}.txt 
        meanTime=0
        for ((i = 0 ; i < $countPasses ; ++i)); do
            > ./Testing/${folder}/out.txt
            PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3 2>&1 > ./Testing/${folder}/out.txt
            meanTime=$(echo "scale=3; $meanTime + ($(cat ./Testing/${folder}/out.txt |grep "Got Result"|awk '{print $2}'))" | bc)
            #echo "time $(cat ./Testing/${folder}/out.txt |grep "Got Result"|awk '{print $2}')"
        done
        meanTime=$(echo "scale=3; $meanTime / $countPasses" | bc)
        #echo "$n $meanTime"
        echo "$n $meanTime" >> ./Testing/${folder}/times-${pcName}-${k}.txt
        #cat ./Testing/${folder}/out.txt |grep "Got Result"
    done
    rm ./instances/${instanceName}${k}.csv


    t1=$(cat ./Testing/${folder}/times-${pcName}-${k}.txt | grep "^1 " | awk '{print $2}')
    > ./Testing/${folder}/relSpeedup-${pcName}-${k}.txt
    > ./Testing/${folder}/efficiency-${pcName}-${k}.txt
    for n in ${npList[@]}; do
        #echo $test
        sRel=$(echo "scale=3; $t1 / $(cat ./Testing/${folder}/times-${pcName}-${k}.txt | grep "^${n} " | awk '{print $2}')" | bc)
        efficiency=$(echo "scale=3; $sRel/$n" | bc)
        echo "$n $sRel" >> ./Testing/${folder}/relSpeedup-${pcName}-${k}.txt
        echo "$n $efficiency" >> ./Testing/${folder}/efficiency-${pcName}-${k}.txt
    done
done
