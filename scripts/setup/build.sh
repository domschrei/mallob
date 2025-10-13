#!/bin/bash

# Only needed if building with -DMALLOB_APP_SAT=1 (enabled by default).
# For non-x86-64 architectures (ARM, POWER9, etc.), prepend `DISABLE_FPU=1` to "bash".
( cd lib && bash fetch_and_build_solvers.sh k sweep)
#
# Build Mallob
# Specify `-DCMAKE_BUILD_TYPE=RELEASE` for a release build or `-DCMAKE_BUILD_TYPE=DEBUG` for a debug build.
# Find all build options at: docs/setup.md
mkdir -p build
cd build
CC=$(which mpicc) CXX=$(which mpicxx) cmake -DCMAKE_BUILD_TYPE=RELEASE \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_ASSERT=1 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

make clean
make -j 20; cd ..


#-DMALLOB_APP_SWEEP=1 \

