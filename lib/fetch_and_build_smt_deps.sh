#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="c0dc3fede7d13ab536af29d5480b32ae69684de7"
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
