#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma-dev-sc26"

# For now, expect a bundled Satsuma ZIP in this directory:
unzip ${dirname}*.zip
for d in $dirname-*/ ; do
    cd "$d"
    mv $(find . -maxdepth 1 -mindepth 1) ../
    cd ..
done
rmdir $dirname-*/

# Once Satsuma version is public, do something like this:
#branchorcommit="b5f37b21385ee802ce015103b23aff62f92b1734" # updated 2026-04-29
#fetch_and_extract $dirname CMakeLists.txt https://github.com/domschrei/impcheck/archive/${branchorcommit}.zip

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
