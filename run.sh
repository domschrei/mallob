#!/bin/bash

if [ "$1" ]; then 
    NP="$1"
else
    NP=9
    echo "Launching with 9 nodes."
fi

mpirun -np "$NP" ./balancer | tee logs/log.`date +%s`
