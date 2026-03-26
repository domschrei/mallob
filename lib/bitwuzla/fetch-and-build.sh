#!/bin/bash

source ../base-build-functions.sh
dirname="bitwuzla"

branchorcommit="1177124ddc7e6ea9ce7eaf464da199d141146f04" # main branch, updated 2026-03-15
fetch_and_extract $dirname configure.py https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
./configure.py --fpexp --no-cadical --no-kissat --no-python --static
cd build
    ninja
cd ..
echo "[$dirname] Build complete"
