#!/bin/bash

set -e

if [ ! -f CMakeLists.txt ]; then
    if [ ! -f palrup.zip ]; then
        echo "[palrup] Fetching sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="bfde5f1c74279a892a85335c1929efde702f211f" # updated 2026-01-29
        curl -L -o palrup.zip https://github.com/rubenGoetz/PalRUP-Check/archive/${branchorcommit}.zip
    fi
    echo "[palrup] Extracting sources ..."
    unzip palrup.zip
    mv PalRUP-Check-*/* PalRUP-Check-*/.* ./ || :
    rmdir PalRUP-Check-*/
    sed -i 's/-Werror//g' CMakeLists.txt
else
    echo "[palrup] Assuming sources are present"
fi

echo "[palrup] Building ..."
mkdir -p build
cd build
cmake ..
make
cd ..
echo "[palrup] Build complete"
