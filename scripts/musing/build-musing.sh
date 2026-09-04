#!/bin/bash
echo "BUILD MALLOB for MUSING"

mkdir -p build
rm build/*mallob*

cd build
export CC=$(which mpicc) 
export CXX=$(which mpicxx) 
cmd=(cmake 
  -DCMAKE_BUILD_TYPE=RELEASE
  -DMALLOB_APP_SAT=1 
  -DMALLOB_LOG_VERBOSITY=4
  -DMALLOB_USE_JEMALLOC=1
  -DMALLOB_ASSERT=1
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\"
  -DMALLOB_APP_INCSAT=0
  -DMALLOB_APP_MAXSAT=0
  -DMALLOB_APP_SMT=0
  -DMALLOB_APP_PALRUPCHECK=0
  -DMALLOB_APP_SWEEP=0
  -DMALLOB_APP_SATWITHPRE=0
  -DMALLOB_ASSERT_HEAVY=0
  -DMALLOB_USE_ASAN=0
  ..)

echo "COMMAND: ${cmd[*]}"
${cmd[@]}
make -j 12
cd ..

