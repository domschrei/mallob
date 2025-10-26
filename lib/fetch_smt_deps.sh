#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="35a785c183e07b98d0c426e7cf0e8a4cf78a685b" # external-sat-solver branch, updated 2025-10-26
        wget -nc https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    unzip bitwuzla.zip
    mv bitwuzla-* bitwuzla
fi
