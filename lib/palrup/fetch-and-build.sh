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
echo "[$dirname] Build complete"
