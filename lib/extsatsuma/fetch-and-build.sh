#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma"

branchorcommit="7ca68dbf2e18f818ee81f2350e4a8f8efa3c9274"
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
    echo "[$dirname] cp build/run-satsuma.sh $1/"
    cp build/run-satsuma.sh "$1/"
fi
