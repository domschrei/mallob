#!/bin/bash

set -e

bash fetch_smt_deps.sh

cd bitwuzla
    ./configure.py --fpexp
    cd build
        ninja
    cd ..
cd ..
