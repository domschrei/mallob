#!/bin/bash
> ./Testing/${1}/err.txt
while true; do
    #echo "$(cat ./Testing/${1}/out.txt | grep "ERROR")"
    echo "$(cat ./Testing/${1}/out.txt | grep "c 10000." | head -1 | awk '{print $4}')"
    sleep 5
    if [ "$(cat ./Testing/${1}/out.txt | grep "c 10000." | head -1 | awk '{print $4}')" == "sysstate" ] || [ "$(cat ./Testing/${1}/out.txt | grep "ERROR" | head -1 | awk '{print $4}')" == "[ERROR]" ]; then
        echo "found err"
        cat ./Testing/${1}/out.txt >> ./Testing/${1}/err.txt
         echo "_________________________________________________________________" >> ./Testing/${1}/err.txt
        killall -9 mpirun ; killall -9 build/mallob
    fi
done
