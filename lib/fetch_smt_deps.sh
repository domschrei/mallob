#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="40489f8f16c7c467e8f246a7201ddea8885e4daa" # external-sat-solver branch, updated 2025-10-23
        wget -nc https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    unzip bitwuzla.zip
    mv bitwuzla-* bitwuzla
fi
