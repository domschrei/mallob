#!/bin/bash

set -e

if [ ! -f CMakeLists.txt ]; then
    if [ ! -f palrup.zip ]; then
        echo "[palrup] Fetching sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="5fc5c7577e723848934fbb03b59836f76be956f5" # updated 2026-01-29
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

if ! [ -z "$1" ]; then
    for x in local_check redistribute confirm; do
        echo "[palrup] cp build/palrup_$x $1/palrup_$x"
        cp build/palrup_$x "$1/palrup_$x"
    done

    for x in "" _launcher; do
        echo "[palrup] cp build/pal${x}.sh $1/pal${x}.sh"
        cp build/pal${x}.sh "$1/pal${x}.sh"
    done

    cp build/out.palrup_import.dummy "$1/out.palrup_import.dummy"
fi
