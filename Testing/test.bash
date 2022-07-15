#!/bin/bash
pcName="i10pc135"
folder="latestResults"
instanceName="benign_traffic" #     mnist784       covtypeShuffle
instanceFirstLine="115 115 52150" # 7 7 209         54 55 581012
kList=(5 10 20 30 40 50 60 70 80) # 
npList=(8 4 2 1) #
for k in ${kList[@]}; do
    echo "$k $instanceFirstLine" > ./instances/${instanceName}${k}.csv
    cat ./instances/${instanceName}K.csv >> ./instances/${instanceName}${k}.csv
    > ./Testing/${folder}/times-${pcName}-${k}.txt
    for n in ${npList[@]}; do
        # v=3 
        #PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3
        # v=0 >> ./Testing/times-${k}.txt 
        PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/${instanceName}${k}.csv -v=3 > ./Testing/${folder}/out.txt
        cat ./Testing/${folder}/out.txt |grep "Got Result"|awk '{print '$n',$2}' >> ./Testing/${folder}/times-${pcName}-${k}.txt
        cat ./Testing/${folder}/out.txt |grep "Got Result"
    done
    rm ./instances/${instanceName}${k}.csv
    t1=$(cat ./Testing/${folder}/times-${pcName}-${k}.txt | grep "1 " | awk '{print $2}')
    > ./Testing/${folder}/relSpeedup-${pcName}-${k}.txt
    > ./Testing/${folder}/efficiency-${pcName}-${k}.txt
    for n in ${npList[@]}; do
        #echo $test
        sRel=$(echo "scale=2; $t1 / $(cat ./Testing/${folder}/times-${pcName}-${k}.txt | grep "^${n} " | awk '{print $2}')" | bc)
        efficiency=$(echo "scale=2; $sRel/$n" | bc)
        echo "$n $sRel" >> ./Testing/${folder}/relSpeedup-${pcName}-${k}.txt
        echo "$n $efficiency" >> ./Testing/${folder}/efficiency-${pcName}-${k}.txt
    done
done

