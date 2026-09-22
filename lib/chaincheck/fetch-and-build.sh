#!/bin/bash

source ../base-build-functions.sh
dirname="chaincheck"

branchorcommit="e740c5180781364afef3048f27ffe4fea15d01ac" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/domschrei/chaincheck/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
cmake -B build
cmake --build build
echo "[$dirname] Build complete"

if ! [ -z "$1" ]; then
    echo "[$dirname] cp build/proof_checker $1/chaincheck"
    cp build/proof_checker "$1/chaincheck"
fi
