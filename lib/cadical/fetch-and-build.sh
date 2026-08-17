#!/bin/bash

source ../base-build-functions.sh
dirname="cadical"

branchorcommit="be7a0f84190b3216c589696b2010e8cbf8a8252e" # updated 2026-02-05
fetch_and_extract $dirname configure https://github.com/domschrei/cadical/archive/${branchorcommit}.zip

echo "[cadical] Building ..."
./configure
make -j
echo "[cadical] Build complete"
