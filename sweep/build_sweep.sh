#!/bin/bash

#get the latest swissat source & binary
./sweep/fetch-local-sweep-kissat.sh

mkdir -p build
#remove old binaries and linkers to make sure they are updated
rm build/*mallob*

cd build
CC=$(which mpicc) CXX=$(which mpicxx) cmake -DCMAKE_BUILD_TYPE=RELEASE \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_APP_SATWITHPRE=1 \
  -DMALLOB_APP_SWEEP=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_ASSERT=1 \
  -DMALLOB_ASSERT_HEAVY=0 \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

# make clean
make -j20; cd ..



# MALLOB_OPTIONS="-DCMAKE_BUILD_TYPE=RELEASE \
#   -DMALLOB_USE_ASAN=1 \ 
#   -DMALLOB_ASSERT=1 \
#   -DMALLOB_ASSERT_HEAVY=0 \
#   -DMALLOB_USE_JEMALLOC=1  \
#   -DMALLOB_JEMALLOC_DIR='$HOME/repos/jemalloc/lib' \
#   -DMALLOB_LOG_VERBOSITY=4 \
#   -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" \
#   -DMALLOB_USE_GLUCOSE=0 \
#   -DMALLOB_BUILD_LRAT_MODULES=0 \
#   -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 \
