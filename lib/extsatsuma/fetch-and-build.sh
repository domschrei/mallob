#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma"

branchorcommit="16d6b0c889f849ef6af17849c0df933ee49b1aef"
fetch_and_extract $dirname CMakeLists.txt https://github.com/domschrei/satsuma/archive/${branchorcommit}.zip

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
