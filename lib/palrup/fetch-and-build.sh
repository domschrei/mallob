#!/bin/bash

source ../base-build-functions.sh
dirname="PalRUP-Check"

branchorcommit="9357fd160ac53e81cc701e6d2616617bd774d6c0" # updated 2026-08-20
fetch_and_extract $dirname CMakeLists.txt https://github.com/rubenGoetz/PalRUP-Check/archive/${branchorcommit}.zip

sed -i 's/-Werror//g' CMakeLists.txt

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake ..
make
cd ..
echo "[$dirname] Build complete"
