#!/bin/bash
kList=(5 10 20 30 40) # 
npList=(128 64 32 16 8 4 2 1) #
for k in ${kList[@]}; do
    > ./Testing/times-${k}.txt
    for n in ${npList[@]}; do
        # v=3 
        #PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/covtypeShuffle${k}.csv -v=3
        # v=0 >> ./Testing/times-${k}.txt 
        PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np ${n} -oversubscribe build/mallob -mono-application=KMEANS -mono=./instances/covtypeShuffle${k}.csv -v=0 |grep "Got Result"|awk '{print '$n',$2}' >> ./Testing/times-${k}.txt
    done
done

