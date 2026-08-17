#!/bin/bash

source ../base-build-functions.sh
dirname="bitwuzla"

branchorcommit="5027ca9a598cb6d5cf392767c80c77e0478d86bd" # main branch, updated 2026-08-17
fetch_and_extract $dirname configure.py https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
./configure.py --fpexp --no-cadical --no-kissat --no-python --static
cd build
    ninja
cd ..
echo "[$dirname] Build complete"
