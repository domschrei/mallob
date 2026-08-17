#!/bin/bash

source ../base-build-functions.sh
dirname="minisat"

branchorcommit="8ac6751aa8530d4e163b79eebccfa2df55832da5"
fetch_and_extract $dirname Makefile https://github.com/domschrei/minisat/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake -DMINISAT_QUIET=1 ..
make
cd ..
echo "[$dirname] Build complete"
