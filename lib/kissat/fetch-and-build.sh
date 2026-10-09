#!/bin/bash

source ../base-build-functions.sh
dirname="kissat"

branchorcommit="9fed5a1226cfee044285c952fc8c509178b539f9" # updated 2026-10-09
fetch_and_extract $dirname configure https://github.com/domschrei/kissat/archive/${branchorcommit}.zip
# Niccos Sweep Model Reconstruction
# branch="update24"   
# fetch_and_extract $dirname configure https://github.com/nrilu/kissat/archive/refs/heads/${branch}.zip

echo "[kissat] Building ..."
./configure -O3
make -j
echo "[kissat] Build complete"
