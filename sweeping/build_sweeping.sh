#!/bin/bash

#get the latest swissat source & binary
./sweeping/fetch-local-sweep-kissat.sh

mkdir -p build
#remove old binaries and linkers, to make sure they are updated
rm build/*mallob*

cd build
CC=$(which mpicc) CXX=$(which mpicxx) cmake -DCMAKE_BUILD_TYPE=RELEASE \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_APP_SATWITHPRE=1 \
  -DMALLOB_APP_SWEEP=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

# make clean
make -j20; cd ..



