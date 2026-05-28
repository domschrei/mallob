#!/bin/bash

source ../base-build-functions.sh
dirname="bitwuzla"

branchorcommit="af2f6029d4cf978decb7652b4902210998001e1a" # main branch, updated 2026-05-28
fetch_and_extract $dirname configure.py https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
./configure.py --fpexp --no-cadical --no-kissat --no-python --static
cd build
    ninja
cd ..
echo "[$dirname] Build complete"
