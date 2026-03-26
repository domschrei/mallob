#!/bin/bash

source ../base-build-functions.sh
dirname="kissat"

branchorcommit="b6871cff6001e299099f07df3b9c73af78a07d9c" # updated 2026-01-29
fetch_and_extract $dirname configure https://github.com/domschrei/kissat/archive/${branchorcommit}.zip

echo "[kissat] Building ..."
./configure -O3 --no-proofs
make -j
echo "[kissat] Build complete"
