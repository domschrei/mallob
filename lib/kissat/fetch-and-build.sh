#!/bin/bash

set -e

if [ ! -f configure ]; then
    if [ ! -f kissat.zip ]; then
        echo "Fetching solver sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="b6871cff6001e299099f07df3b9c73af78a07d9c" # updated 2026-01-29
        curl -L -o kissat.zip https://github.com/domschrei/kissat/archive/${branchorcommit}.zip
        #wget -nc https://github.com/domschrei/kissat/archive/${branchorcommit}.zip -O kissat.zip
    fi
    echo "Extracting solver sources ..."
    unzip kissat.zip
    mv kissat-*/* kissat-*/.* ./
    rmdir kissat-*/
else
    echo "Assuming solver sources are present"
fi

echo "Building"
./configure -O3 --no-proofs
make -j
echo "Solver built"
