#!/bin/bash
#
set -e
BUILD_PROCS=10

if [ $(hostname) = "vvbk" ]; then
  ./sweep/fetch-local-sweep-kissat.sh
else
  module load slurm_setup
  module unload devEnv/Intel/2019 intel-mpi cmake
  module load gcc/11 intel-mpi/2019-gcc cmake/3.21.4 gdb valgrind
  echo "Modules loaded"
  ( cd lib && bash fetch_and_build_solvers.sh klyc sweep)
  BUILD_PROCS=20
fi

mkdir -p build
rm -f build/*mallob* 2>/dev/null || true

BUILD_TYPE="DEBUG"
if [ "$1" = "release" ] || [ "$1" = "rel" ]; then
  BUILD_TYPE="RELEASE"
fi

echo "Building $BUILD_TYPE"

BUILD_OPTIONS=(
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DMALLOB_APP_SAT=1
  -DMALLOB_APP_SATWITHPRE=1
  -DMALLOB_APP_SWEEP=1
  -DMALLOB_LOG_VERBOSITY=4
  -DMALLOB_ASSERT=1
  -DMALLOB_USE_JEMALLOC=1
  -DMALLOB_JEMALLOC_DIR="$HOME/jemalloc-5.2.1/lib/"
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\"
  -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64
)

# Add AddressSanitizer only for debug builds
# if [[ "$BUILD_TYPE" == "DEBUG" ]]; then
  # BUILD_OPTIONS+=(-DMALLOB_USE_ASAN=1) #created too many warnings/errors everywhere !?
# fi

cd build
CC=$(which mpicc) 
CXX=$(which mpicxx) 
cmake "${BUILD_OPTIONS[@]}" ..
# cmake $BUILD_OPTIONS
make clean
make -j $BUILD_PROCS; cd ..


#-DMALLOB_APP_SWEEP=1 \

