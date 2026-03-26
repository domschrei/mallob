#!/bin/bash

source ../base-build-functions.sh
dirname="impcheck"

branchorcommit="bf740494de4408817f225b90c6ec1d457e8db9b5" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/domschrei/impcheck/archive/${branchorcommit}.zip

echo "[iimpcheck] Building ..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0 -DIMPCHECK_COMPRESS=1
make
cd ..
echo "[iimpcheck] Build complete"

if ! [ -z "$1" ]; then
    for x in parse check confirm; do
        echo "[iimpcheck] cp build/impcheck_$x $1/iimpcheck_$x"
        cp build/impcheck_$x "$1/iimpcheck_$x"
    done
fi
