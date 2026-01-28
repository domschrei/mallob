#!/bin/bash

set -e

cd lib
  bash fetch_and_build_solvers.sh kcly

  if [ ! -d impcheck ]; then git clone git@github.com:domschrei/impcheck.git ; fi
  cd impcheck
    git checkout incremental
    mkdir -p build ; cd build
      cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0 -DIMPCHECK_COMPRESS=1 ; make -j
    cd ..
  cd ..
  cp impcheck/build/impcheck_* ../build/
cd ..

