#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma"

branchorcommit="9f8032a792149978b1b9868cb4b7baba551adf55"
fetch_and_extract $dirname CMakeLists.txt https://github.com/domschrei/satsuma-with-cliquer/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=RELEASE ..
make -j
cd ..
echo "[$dirname] Build complete"

if ! [ -z "$1" ]; then
    echo "[$dirname] cp build/satsuma $1/"
    cp build/satsuma "$1/"
fi
