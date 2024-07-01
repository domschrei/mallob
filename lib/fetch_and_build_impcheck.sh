#!/bin/bash

set -e

if [ ! -d impcheck ]; then
    git clone https://github.com/domschrei/impcheck.git
fi

cd impcheck

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0
make
cd ..
