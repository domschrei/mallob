#!/bin/bash

echo "Usage: source scripts/server/create_mallob_env.sh [--fresh]"

source /nfs/software/setup.sh  #Might be necessary to bootstrap spack itself if not already on the login node

create=0

if ! spack env list | grep -q mallob_env; then
    echo "Spack: creating mallob_env"
    spack env create mallob_env
    create=1
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
	    create=1
	fi
    else
	echo "Skipping env creation, directly activating it"
	echo "For a fresh install, rerun with --fresh"
    fi
fi


if [ "$create" -eq 1 ]; then

	spack env activate mallob_env
	spack add cmake gcc jemalloc openmpi curl gdb meson
	echo "Installing. Might take 1-2min."
	spack concretize
	spack install -j 32

fi

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
	echo MESON
	echo $(meson --version)
	echo "" 

spack env activate mallob_env
echo "(script) Activated mallob_env"
