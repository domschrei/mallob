#!/bin/bash

set -e

if [ ! -f CMakeLists.txt ]; then
    if [ ! -f impcheck.zip ]; then
        echo "Fetching sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="a96fb5f268e7c384dafbf2861deeeb4ac8330146" # updated 2026-01-29
        curl -L -o impcheck.zip https://github.com/tanyongkiam/impcheck/archive/${branchorcommit}.zip
    fi
    echo "Extracting sources ..."
    unzip impcheck.zip
    mv impcheck-*/* impcheck-*/.* ./
    rmdir impcheck-*/
else
    echo "Assuming sources are present"
fi

echo "Building"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0
make
cd ..
echo "ImpCake built"

if ! [ -z "$1" ]; then
    for x in parse check confirm; do
        echo cp build/impcheck_$x "$1/impcake_$x"
        cp build/impcheck_$x "$1/impcake_$x"
    done
fi
