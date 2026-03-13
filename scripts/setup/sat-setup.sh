#!/bin/bash

set -e

cd lib
  bash fetch_and_build_solvers.sh kcly

  if [ ! -d iimpcheck ]; then git clone git@github.com:domschrei/impcheck.git iimpcheck ; fi
  cd iimpcheck
    git checkout incremental
    mkdir -p build ; cd build
      cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0 -DIMPCHECK_COMPRESS=1 ; make -j
    cd ..
  cd ..
  mkdir -p ../build/
  cp iimpcheck/build/impcheck_parse ../build/iimpcheck_parse
  cp iimpcheck/build/impcheck_check ../build/iimpcheck_check
  cp iimpcheck/build/impcheck_confirm ../build/iimpcheck_confirm
cd ..

