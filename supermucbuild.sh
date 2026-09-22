#!/bin/bash
source load_standard_modules.sh
export CC=$(which mpicc) 
export CXX=$(which mpicxx) 


mkdir -p build
rm build/*mallob*

cd build
echo $CC
echo $CXX


cmake -DCMAKE_BUILD_TYPE=RELEASE \
	-DMALLOB_APP_SMT=0 \
	-DMALLOB_APP_MAXSAT=0 \
    -DMALLOB_LOG_VERBOSITY=3 \
    -DMALLOB_ASSERT=1 \
    -DMALLOB_USE_JEMALLOC=1 \
    -DMALLOB_JEMALLOC_DIR="$HOME/jemalloc-5.2.1/lib/" \
    -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

# make clean
make -j
cd ..

