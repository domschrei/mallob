#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma"

branchorcommit="4e2a70714b8ab53557dafe7aa367fc7dc2fe8ee2"
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
