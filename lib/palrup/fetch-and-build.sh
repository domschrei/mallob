#!/bin/bash

source ../base-build-functions.sh
dirname="PalRUP-Check"

branchorcommit="31f52fd70d39c1a6f7eeb3ce1bb7d97dbee6142d" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/rubenGoetz/PalRUP-Check/archive/${branchorcommit}.zip

sed -i 's/-Werror//g' CMakeLists.txt

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake ..
make
cd ..
<<<<<<< HEAD
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
=======
echo "[$dirname] Build complete"
>>>>>>> upstream/fullcmake
