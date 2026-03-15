#!/bin/bash

set -e

if [ ! -f Makefile ]; then
    if [ ! -f minisat.zip ]; then
        echo "[minisat] Fetching sources ..."
        branchorcommit="b4edffa0874eef46c9d1e8e829b92902ec63e6a7"
        curl -L -o minisat.zip https://github.com/domschrei/minisat/archive/${branchorcommit}.zip
    fi
    echo "[minisat] Extracting sources ..."
    unzip minisat.zip
    mv minisat-*/* minisat-*/.* ./
    rmdir minisat-*/
else
    echo "[minisat] Assuming sources are present"
fi

echo "[minisat] Building ..."
mkdir -p build
cd build
cmake -DMINISAT_QUIET=1 ..
make
cd ..
echo "[minisat] Build complete"
