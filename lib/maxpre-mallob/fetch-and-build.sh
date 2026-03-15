#!/bin/bash

set -e

if [ ! -f Makefile ]; then
    echo "[maxpre] Fetching sources ..."
    if [ ! -f maxpre-mallob.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="ccd759aab16dae3d8e74f021d0955d17c424ed38"
        wget -nc https://github.com/jezberg/maxpre-mallob/archive/${branchorcommit}.zip -O maxpre-mallob.zip
    fi
    echo "[maxpre] Extracting sources ..."
    unzip maxpre-mallob.zip
    mv maxpre-mallob-*/* maxpre-mallob-*/.* ./
    rmdir maxpre-mallob-*/
else
    echo "[maxpre] Assuming sources are present"
fi

echo "[maxpre] Building ..."
make lib with_zlib=false
echo "[maxpre] Build complete"
