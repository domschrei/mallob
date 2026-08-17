#!/bin/bash

source ../base-build-functions.sh
dirname="impcheck"

branchorcommit="a96fb5f268e7c384dafbf2861deeeb4ac8330146" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/tanyongkiam/impcheck/archive/${branchorcommit}.zip

echo "[impcake] Building ..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0
make
cd ..
echo "[impcake] Build complete"

if ! [ -z "$1" ]; then
    for x in parse check confirm; do
        echo "[impcake] cp build/impcheck_$x $1/impcake_$x"
        cp build/impcheck_$x "$1/impcake_$x"
    done
fi
