#!/bin/bash

source ../base-build-functions.sh
dirname="PalRUP-Check"

branchorcommit="d089fd6dd4425f56fb8c62ea96cf2be495b41051" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/rubenGoetz/PalRUP-Check/archive/${branchorcommit}.zip

sed -i 's/-Werror//g' CMakeLists.txt

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake ..
make
cd ..
echo "[$dirname] Build complete"
