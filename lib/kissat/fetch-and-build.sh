#!/bin/bash

source ../base-build-functions.sh
dirname="kissat"

#TODO: Needs to be updated to the new Kissat with Sweep-Code merged
branchorcommit="b6871cff6001e299099f07df3b9c73af78a07d9c" # updated 2026-01-29
fetch_and_extract $dirname configure https://github.com/domschrei/kissat/archive/${branchorcommit}.zip
# Niccos Sweep Kissat
# branchorcommit="d4e76a387c93b28bbe84a2db043008e5bc70b185" #FMCAD26 Artifact Commit
# fetch_and_extract $dirname configure https://github.com/nrilu/kissat/archive/${branchorcommit}.zip

echo "[kissat] Building ..."
./configure -O3 --no-proofs
make -j
echo "[kissat] Build complete"
