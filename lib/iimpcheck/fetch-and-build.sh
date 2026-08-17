#!/bin/bash

source ../base-build-functions.sh
dirname="impcheck"

branchorcommit="b5f37b21385ee802ce015103b23aff62f92b1734" # updated 2026-04-29
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
