#!/bin/bash

# TODO Load your own suitable modules here
module load slurm_setup
module unload devEnv/Intel/2019 intel-mpi cmake
module load gcc/11 intel-mpi/2019-gcc cmake/3.21.4 gdb valgrind
echo "Modules loaded"

( cd lib && bash fetch_and_build_solvers.sh klyc sweep)

mkdir -p build
rm build/*mallob*

cd build

CC=$(which mpicc) 
CXX=$(which mpicxx) 
cmake -DCMAKE_BUILD_TYPE=RELEASE \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_APP_SATWITHPRE=1 \
  -DMALLOB_APP_SWEEP=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_ASSERT=1 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 \
  -DMALLOB_JEMALLOC_DIR="$HOME/jemalloc-5.2.1/lib/" \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

make clean
make -j 20; cd ..

