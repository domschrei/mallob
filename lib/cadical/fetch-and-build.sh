#!/bin/bash

set -e

if [ ! -f configure ]; then
    echo "[cadical] Fetching solver sources ..."
    if [ ! -f cadical.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="be7a0f84190b3216c589696b2010e8cbf8a8252e" # updated 2026-02-05
        curl -L -o cadical.zip https://github.com/domschrei/cadical/archive/${branchorcommit}.zip
    fi
    echo "[cadical] Extracting sources ..."
    unzip cadical.zip
    mv cadical-*/* cadical-*/.* ./
    rmdir cadical-*/
else
    echo "[cadical] Assuming sources are present"
fi

echo "[cadical] Building ..."
./configure
make -j
echo "[cadical] Build complete"
