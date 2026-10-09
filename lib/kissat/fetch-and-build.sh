#!/bin/bash

source ../base-build-functions.sh
dirname="kissat"

# branchorcommit="b0b8b6c259cba99cdb3af004cc55f71386525d68" # updated 2026-09-02
# fetch_and_extract $dirname configure https://github.com/domschrei/kissat/archive/${branchorcommit}.zip
# Niccos Sweep Model Reconstruction
branch="update24"   
fetch_and_extract $dirname configure https://github.com/nrilu/kissat/archive/refs/heads/${branch}.zip

echo "[kissat] Building ..."
./configure -O3
make -j
echo "[kissat] Build complete"
