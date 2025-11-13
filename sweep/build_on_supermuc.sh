#!/bin/bash

module load slurm_setup
module unload devEnv/Intel/2019 intel-mpi cmake
module load gcc/11 intel-mpi/2019-gcc cmake/3.21.4 gdb valgrind
echo "Modules loaded"


echo "---"
echo "REMOVING OLD KISSAT, forcing fetching of newest"
echo "---"
rm -r lib/kissat
rm lib/kissat.zip
( cd lib && bash fetch_and_build_solvers.sh klyc sweep)

mkdir -p build
rm build/*mallob*

cd build
CC=$(which mpicc) 
CXX=$(which mpicxx) 

echo $CC
echo $CXX

if [ "$1" = "deb" ] || [ "$1" = "debug" ]; then
  echo "Building Mallob DEBUG"
  cmake -DCMAKE_BUILD_TYPE=DEBUG \
    -DMALLOB_APP_SAT=1 \
    -DMALLOB_APP_SATWITHPRE=1 \
    -DMALLOB_APP_SWEEP=1 \
    -DMALLOB_LOG_VERBOSITY=5 \
    -DMALLOB_ASSERT=1 \
    -DMALLOB_USE_JEMALLOC=1 \
    -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 \
    -DMALLOB_JEMALLOC_DIR="$HOME/jemalloc-5.2.1/lib/" \
    -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..
else
  echo "Building Mallob RELEASE"
  cmake -DCMAKE_BUILD_TYPE=RELEASE \
    -DMALLOB_APP_SAT=1 \
    -DMALLOB_APP_SATWITHPRE=1 \
    -DMALLOB_APP_SWEEP=1 \
    -DMALLOB_LOG_VERBOSITY=5 \
    -DMALLOB_ASSERT=1 \
    -DMALLOB_USE_JEMALLOC=1 \
    -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 \
    -DMALLOB_JEMALLOC_DIR="$HOME/jemalloc-5.2.1/lib/" \
    -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..
fi

make clean
make -j 30; cd ..

