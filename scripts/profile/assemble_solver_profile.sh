#!/bin/bash

if [ -z "$@" ]; then
    echo "Usage: assemble_solver_profile.sh (Mallob CaDiCaL profiling output files) > profile.txt"
fi

for f in $@ ; do
    slv=$(echo $f|grep -oE "[0-9]+$")
    if [ -f "$(dirname $f)/instance.txt" ]; then
        inst=$(cat $(dirname $f)/instance.txt|sed 's,.*/,,g')
    else
        inst="?"
    fi
    cat $f | sed 's/%//g' | awk 'NF==3 {print "'$inst'","'$slv'",$3,$1,0.01*$2}'
done
