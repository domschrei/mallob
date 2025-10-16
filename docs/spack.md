# Compiling Mallob in a spack environment 

On a machine that uses [spack](https://spack.io/SPACK) for package management, Mallob can be compiled in the following way.

    
    if ! spack env list | grep -q mallob_env; then
        echo "Creating Spack environment mallob_env"
        spack env create mallob_env
    fi
    
    spack env activate mallob_env
    spack config add "packages:elfutils:variants: ~debuginfod"
    spack add cmake gcc jemalloc openmpi curl gdb
    spack concretize
    spack install -j 32
    
    #gdb compilation crashes without this elfutils:variants and curl
    
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
  
