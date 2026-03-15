#!/bin/bash

set -e

if [ ! -f CMakeLists.txt ]; then
    if [ ! -f impcheck.zip ]; then
        echo "Fetching sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="bf740494de4408817f225b90c6ec1d457e8db9b5" # updated 2026-01-29
        curl -L -o impcheck.zip https://github.com/domschrei/impcheck/archive/${branchorcommit}.zip
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
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0 -DIMPCHECK_COMPRESS=1
make
cd ..
echo "IImpCheck built"

if ! [ -z "$1" ]; then
    for x in parse check confirm; do
        echo cp build/impcheck_$x "$1/iimpcheck_$x"
        cp build/impcheck_$x "$1/iimpcheck_$x"
    done
fi
