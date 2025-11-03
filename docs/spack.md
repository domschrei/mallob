# Using Mallob on a server with the  spack environment 

On machines that use [spack](https://spack.readthedocs.io/en/latest/index.html) for package management (for example our custom servers), Mallob can be compiled in the following way. 

First, on the login node, run once 
    
    source scripts/spack/create_mallob_env.sh 

This create a spack environment ```mallob_env``` containing all necessary compilers and libraries (see [create_mallob_env.sh](/scripts/spack/create_mallob_env.sh)). In case you want to update or extend the environment, rerun this script with the flag ```--fresh```, this forces a complete reinstall and is more robust than an incremental addition.

Next, we create a script that compiles Mallob and runs it on some simple instances.  

To be executed from the home ```mallob``` directory.
TODO:

    
    source /nfs/software/setup.sh
    
    if ! spack env list | grep -q mallob_env; then
        echo "Spack environment mallob_env is missing, you must create that first! "
        return 0
    fi


    
    spack env activate mallob_env
    
    ( cd lib && bash fetch_and_build_solvers.sh kcly )
    
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
  
