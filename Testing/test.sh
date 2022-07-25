#!/bin/bash
pcName="i10pc135"
folder="dTest" #latestResults
instanceName="covtypeShuffle" #   mnist784      benign_traffic      covtypeShuffle
instanceFirstLine="54 55" #7 7 209    115 115 52150      54 55 581012
kList=(30) # 60 70 80
npList=(160 155 150 145 140 135 130 128 127 125 120 115 110 105 100 95 90 85 80 75 70 65 64 63 60 55 50 45 40 35 32 31 30 25 20 16 15 10 8 7 5 4 3 2 1) #   65 64 63 60 55 50 45 40 35 32 31 30 25 20 16 15 10 8 7 5 4 3 2 1
dList=(10000 100000 250000 500000)
countPasses=2

for k in ${kList[@]}; do
    for d in ${dList[@]}; do


        echo "$k $instanceFirstLine $d" > ./instances/${instanceName}${k}.csv
        cat ./instances/${instanceName}K.csv >> ./instances/${instanceName}${k}.csv
        > ./Testing/${folder}/times-${pcName}-${k}-${d}.txt
        > ./Testing/${folder}/out.txt
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
            echo "$n $meanTime" >> ./Testing/${folder}/times-${pcName}-${k}-${d}.txt
            #cat ./Testing/${folder}/out.txt |grep "Got Result"
        done
        rm ./instances/${instanceName}${k}.csv


        t1=$(cat ./Testing/${folder}/times-${pcName}-${k}-${d}.txt | grep "^1 " | awk '{print $2}')
        > ./Testing/${folder}/relSpeedup-${pcName}-${k}-${d}.txt
        > ./Testing/${folder}/efficiency-${pcName}-${k}-${d}.txt
        for n in ${npList[@]}; do
            #echo $test
            sRel=$(echo "scale=3; $t1 / $(cat ./Testing/${folder}/times-${pcName}-${k}-${d}.txt | grep "^${n} " | awk '{print $2}')" | bc)
            efficiency=$(echo "scale=3; $sRel/$n" | bc)
            echo "$n $sRel" >> ./Testing/${folder}/relSpeedup-${pcName}-${k}-${d}.txt
            echo "$n $efficiency" >> ./Testing/${folder}/efficiency-${pcName}-${k}-${d}.txt
        done
    done
done