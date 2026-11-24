#!/bin/bash

source ../base-build-functions.sh
dirname="bitwuzla"

branchorcommit="f8a2eaba6185700d0900bb0d3da279d3e3ce1038" # main branch, updated 2026-08-17
fetch_and_extract $dirname configure.py https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
./configure.py --fpexp --no-cadical --no-kissat --no-python --static
cd build
    ninja
cd ..
echo "[$dirname] Build complete"
