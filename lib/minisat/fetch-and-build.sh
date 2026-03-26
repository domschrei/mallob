#!/bin/bash

source ../base-build-functions.sh
dirname="minisat"

branchorcommit="b4edffa0874eef46c9d1e8e829b92902ec63e6a7"
fetch_and_extract $dirname Makefile https://github.com/domschrei/minisat/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake -DMINISAT_QUIET=1 ..
make
cd ..
echo "[$dirname] Build complete"
