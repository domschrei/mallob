#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="bd60c2f9a93d3d6fb98f28e10bb308f637146100" # main branch, updated 2025-06-25
        wget -nc https://github.com/domschrei/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    unzip bitwuzla.zip
    mv bitwuzla-* bitwuzla
fi
cd bitwuzla
    ./configure.py
    cd build
        ninja
    cd ..
cd ..
