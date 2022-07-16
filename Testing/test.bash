#!/bin/bash
pcName="i10pc135"
folder="latestResults"
instanceName="benign_traffic" #   mnist784            covtypeShuffle
instanceFirstLine="115 115 52150" #7 7 209          54 55 581012
kList=(30 40) # 5 10 20  50 60 70 80
npList=(4 3 2 1) #128 127 64 63 32 31 16 15 8 7 
countPasses=3
for k in ${kList[@]}; do
    echo "$k $instanceFirstLine" > ./instances/${instanceName}${k}.csv
    cat ./instances/${instanceName}K.csv >> ./instances/${instanceName}${k}.csv
    #> ./Testing/${folder}/times-${pcName}-${k}.txt
    for n in ${npList[@]}; do
        # v=3 
        #PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3
        # v=0 >> ./Testing/times-${k}.txt 
        meanTime=0
        for ((i = 0 ; i < $countPasses ; ++i)); do
            > ./Testing/${folder}/out.txt
            PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3 > ./Testing/${folder}/out.txt
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

