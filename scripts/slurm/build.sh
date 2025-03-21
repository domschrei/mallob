#!/bin/bash

# TODO Load your own suitable modules here
module load slurm_setup
module unload devEnv/Intel/2019 intel-mpi cmake
module load gcc/11 intel-mpi/2019-gcc cmake/3.21.4 gdb valgrind
echo "Modules loaded"

build="build" # TODO set your build directory

mkdir -p $build
cd $build

# TODO Go to lib/ and execute bash fetch_and_build_solvers.sh
# TODO (If jemalloc is not installed at your system) Go to lib/ and execute:
#      wget https://github.com/jemalloc/jemalloc/archive/refs/tags/5.2.1.zip -O jemalloc.zip
#      unzip jemalloc.zip
#      cd jemalloc-*/ && ./autogen.sh && make dist && make && make install
# TODO (MaxSAT) Go to lib/ and execute bash fetch_and_build_maxsat_deps.sh

CC=$(which mpicc) CXX=$(which mpicxx) cmake .. \
`# build type (assertions recommended!)` -DCMAKE_BUILD_TYPE=RELEASE -DMALLOB_ASSERT=1 \
`# jemalloc (provide path if not installed system-wide)` -DMALLOB_USE_JEMALLOC=1 -DMALLOB_JEMALLOC_DIR=lib/jemalloc-5.2.1/lib/ \
`# static maximum logging verbosity` -DMALLOB_LOG_VERBOSITY=4 \
`# (relative) path to sub-executables` -DMALLOB_SUBPROC_DISPATCH_PATH=\"$build/\" \
`# include SAT (+preprocess) as application` -DMALLOB_APP_SAT=1 -DMALLOB_APP_SATWITHPRE=1 \
`# include MaxSAT as application` -DMALLOB_APP_MAXSAT=1 -DMALLOB_USE_MAXPRE=0 \
`# max. number of solver threads in a process` -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64

VERBOSE=1 make -j 8
cd ..
