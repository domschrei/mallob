echo "SPACK environment activation"
spack env activate mallob_env
spack add cmake gcc jemalloc openmpi curl gdb
spack concretize
spack install -j 32
echo "SPACK environment installed"

#curl needed for gdb

mkdir -p build
rm build/*mallob*
cd build
# with USE_JEMALLOC=0, the linker won't find lz and adding libz results in spack compiling every library like gcc what takes pretty long
# change MY_USER_NAME in next command
CC=$(which mpicc) 
CXX=$(which mpicxx) 

cmake -DMALLOB_JEMALLOC_DIR=/nfs/home/$USER/.user_spack/environments/mallob_env/.spack-env/view/lib \
  -DCMAKE_BUILD_TYPE=DEBUG \
  -DMALLOB_APP_SAT=1 \
  -DMALLOB_APP_SATWITHPRE=1 \
  -DMALLOB_APP_SWEEP=1 \
  -DMALLOB_USE_JEMALLOC=1 \
  -DMALLOB_LOG_VERBOSITY=4 \
  -DMALLOB_ASSERT=1 \
  -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..

make clean
make -j 20
cd ..
