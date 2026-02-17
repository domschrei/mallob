#!/bin/bash

set -e

if [ ! -d bitwuzla ]; then
    if [ ! -f bitwuzla.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="2b685c820e04dbb88edcd33349a5099c0fb24c75" # external-sat-solver-on-main branch, updated 2026-02-04
        wget -nc https://github.com/bitwuzla/bitwuzla/archive/${branchorcommit}.zip -O bitwuzla.zip
    fi
    unzip bitwuzla.zip
    mv bitwuzla-* bitwuzla
fi
