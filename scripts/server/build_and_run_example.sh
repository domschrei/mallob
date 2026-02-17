#!/bin/bash

source /nfs/software/setup.sh  #Load spack here on the target machine

if ! spack env list | grep -q mallob_env; then
    echo "Error: Spack environment mallob_env missing, must be created first"
    return 0
fi

spack env activate mallob_env

echo "Build: Fetching SAT solvers from Github"
(cd lib && bash fetch_and_build_solvers.sh kcly)

echo "Build: Cleaning build/ "
mkdir -p build
rm build/*mallob*
cd build

#with USE_JEMALLOC=0, the linker won't find lz, and adding libz results in spack compiling every library like gcc which takes pretty long

CC=$(which mpicc) 
CXX=$(which mpicxx) 
cmake -DMALLOB_JEMALLOC_DIR=/nfs/home/$USER/.user_spack/environments/mallob_env/.spack-env/view/lib \
  -DCMAKE_BUILD_TYPE=RELEASE \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_ASSERT=1 \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

make clean
make -j 20
cd ..

bash scripts/server/run_example.sh
