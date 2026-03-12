#!/bin/bash

set -e

bash fetch_smt_deps.sh

cd bitwuzla
    ./configure.py --fpexp --no-cadical --no-kissat --no-python --static
    cd build
        ninja
    cd ..
cd ..
