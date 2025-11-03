#!/bin/bash

source /nfs/software/setup.sh  #Might be necessary to boostrap spack itself (if not on login node)

echo "Usage: source scripts/spack/create_mallob_env.sh [--fresh]"

if ! spack env list | grep -q mallob_env; then
    echo "Spack: creating mallob_env"
    spack env create mallob_env
else
    echo "Spack: mallob_env already exists"
    if [ "$1" = "--fresh" ]; then
	if spack env status | grep -q mallob_env; then
    		echo "Spack: deactivating mallob_env"
    		despacktivate
	fi
	echo "Fresh reinstall?"
    	spack env remove mallob_env
	if spack env list | grep -q mallob_env; then
	    echo "Choose not to remove existing mallob_env, end script"
	    return 0
	else 
	    spack env create mallob_env
	fi
    else
	echo "Rerun with --fresh to force fresh install"
	return 0
    fi
fi

spack env activate mallob_env

# spack config add "packages:elfutils:variants: ~debuginfod"   maybe not needed anymore with combined concretization
spack add cmake gcc jemalloc openmpi curl gdb
echo "Installing. Might take 1-2min."
spack concretize
spack install -j 32

#Verify
echo ""
echo GDB
echo $(gdb --version)
echo ""
echo JEMALLOC
echo $(jemalloc-config --version)
echo ""
echo CMAKE
echo $(cmake --version)
echo ""
echo MAKE
echo $(make --version)
echo ""
echo GCC
echo $(gcc --version)
echo "" 

spack env activate mallob_env
