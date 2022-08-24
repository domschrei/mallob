#!/bin/bash
pcName="i10pc138"
folder="nTest256core50dLangsamer" #latestResults
instanceName="covtypeShuffle" #   mnist784      benign_trafficShuffle      covtypeShuffle
instanceFirstLine="54 55" #              7 7 209       115 115 52150              54 55 581012
kList=(30 50) # 60 70 80 70 100
wList=(128 127 125 120 110 100 90 80 75 70 65 64 63 60 55 50 45 40 35 32 31 30 25 20 16) #   65 64 63 60 55 50 45 40 35 32 31 30 25 20 16 15 10 8 7 5 4 3 2 1
nList=(100000 300000) # 500000
countPasses=1

./Testing/killHung.sh ${folder} &
for k in ${kList[@]}; do
    for n in ${nList[@]}; do


        echo "$k $instanceFirstLine $n" > ./instances/${instanceName}${k}.csv
        cat ./instances/${instanceName}K.csv >> ./instances/${instanceName}${k}.csv
        > ./Testing/${folder}/times-${pcName}-${k}-${n}.txt
        > ./Testing/${folder}/out.txt
        for w in ${wList[@]}; do #((w = 158 ; w < 161 ; ++w));
            # v=3 
            #PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${w} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3
            # v=0 >> ./Testing/times-${k}.txt 
            meanTime=0
            for ((i = 0 ; i < $countPasses ; ++i)); do
                > ./Testing/${folder}/out.txt
                PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${w} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3 2>&1 > ./Testing/${folder}/out.txt
                meanTime=$(echo "scale=3; $meanTime + ($(cat ./Testing/${folder}/out.txt |grep "Got Result"|awk '{print $2}'))" | bc)
                #echo "time $(cat ./Testing/${folder}/out.txt |grep "Got Result"|awk '{print $2}')"
            done
            meanTime=$(echo "scale=3; $meanTime / $countPasses" | bc)
            #echo "$w $meanTime"
            echo "$w $meanTime" >> ./Testing/${folder}/times-${pcName}-${k}-${n}.txt
            #cat ./Testing/${folder}/out.txt |grep "Got Result"
        done
        rm ./instances/${instanceName}${k}.csv


        t1=$(cat ./Testing/${folder}/times-${pcName}-${k}-${n}.txt | grep "^1 " | awk '{print $2}')
        > ./Testing/${folder}/relSpeedup-${pcName}-${k}-${n}.txt
        > ./Testing/${folder}/efficiency-${pcName}-${k}-${n}.txt
        while read p; do
            w=$(echo "$p" | awk '{print $1}')
            #echo $test
            sRel=$(echo "scale=3; $t1 / $(cat ./Testing/${folder}/times-${pcName}-${k}-${n}.txt | grep "^${w} " | awk '{print $2}')" | bc)
            efficiency=$(echo "scale=3; $sRel/$w" | bc)
            echo "$w $sRel" >> ./Testing/${folder}/relSpeedup-${pcName}-${k}-${n}.txt
            echo "$w $efficiency" >> ./Testing/${folder}/efficiency-${pcName}-${k}-${n}.txt
        done <./Testing/${folder}/times-${pcName}-${k}-${n}.txt
    done
done

kill -9 `ps -aux | grep "./Testing/killHung.sh" | grep -v grep | awk '{ print $2 }'`
