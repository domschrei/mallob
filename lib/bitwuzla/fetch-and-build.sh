#!/bin/bash

set -e

if [ ! -f configure.py ]; then
    echo "[bitwuzla] Fetching sources ..."

    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="1177124ddc7e6ea9ce7eaf464da199d141146f04" # main branch, updated 2026-03-15
        wget -nc https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    echo "[bitwuzla] Extracting sources ..."
    unzip bitwuzla.zip
    mv bitwuzla-*/* bitwuzla-*/.* ./
    rmdir bitwuzla-*/
else
    echo "[bitwuzla] Assuming sources are present"
fi

echo "[bitwuzla] Building ..."
./configure.py --fpexp --no-cadical --no-kissat --no-python --static
cd build
    ninja
cd ..
echo "[bitwuzla] Build complete"
