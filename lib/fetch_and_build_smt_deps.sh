#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="5e1211dc06a9f61fa96f1ea4d1e433849d8e67ae" # main branch, updated 2025-10-09
        wget -nc https://github.com/domschrei/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    unzip bitwuzla.zip
    mv bitwuzla-* bitwuzla
fi
cd bitwuzla
    ./configure.py --fpexp
    cd build
        ninja
    cd ..
cd ..
