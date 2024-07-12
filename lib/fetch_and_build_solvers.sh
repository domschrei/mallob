#!/bin/bash

set -e

disable_fpu=true
if [ -z $DISABLE_FPU ] || [ x$DISABLE_FPU == x0 ]; then
    disable_fpu=false
elif [ x$DISABLE_FPU != x1 ]; then
    echo "Set DISABLE_FPU to either 0 or 1."
    exit 1
fi

if [ -z $1 ]; then
    solvers="clyk"
    echo "Defaulting to solvers $solvers (supply another string to override solvers to build)"
else
    solvers="$1"
fi

bash fetch_solvers.sh $solvers

# MergeSAT
if echo $solvers|grep -q "m" && [ ! -f mergesat/libmergesat.a ]; then
    echo "Building MergeSAT"

    cd mergesat
    make all -j $(nproc)
    cp build/release/lib/libmergesat.a .
    cd ..
fi

# Glucose
if echo $solvers|grep -q "g" && [ ! -f glucose/libglucose.a ]; then
    echo "Building Glucose ..."
    
    if [ ! -f glucose/.PATCHED ]; then
        # Patch bug in solver
        patch glucose/core/Solver.cc < Glucose_Solver.cc.patch
        # Add INCREMENTAL definition to compile flags
        sed -i 's/ -D __STDC_FORMAT_MACROS/ -D __STDC_FORMAT_MACROS -D INCREMENTAL/g' glucose/mtl/template.mk
        # Fix typo in a preprocessor definition
        sed -i 's/INCREMNENTAL/INCREMENTAL/g' glucose/core/SolverTypes.h
        if $disable_fpu; then
            sed -i 's/#if defined(__linux__)/#if 0/g' glucose/simp/Main.cc
        fi
        touch glucose/.PATCHED
    fi
    
    cd glucose/simp
    make libr
    cp lib.a ../libglucose.a
    cd ../..
fi

# YalSAT
if echo $solvers|grep -q "y" && [ ! -f yalsat/libyals.a ]; then
    echo "Building YalSAT ..."

    cd yalsat
    for f in *.c *.h ; do
        sed -i 's/exit ([01])/abort()/g' $f
    done
    if $disable_fpu; then
        sed -i 's/#ifdef __linux__/#if 0/g' yals.c
    fi
    ./configure.sh
    make
    cd ..
fi

# Lingeling
if echo $solvers|grep -q "l" && [ ! -f lingeling/liblgl.a ]; then
    echo "Building lingeling ..."

    cd lingeling
    for f in *.c *.h ; do
        sed -i 's/exit ([01])/abort()/g' $f
    done
    ./configure.sh
    make
    cd ..
fi

# Kissat
if echo $solvers|grep -q "k" && [ ! -f kissat/libkissat.a ]; then
    echo "Building Kissat ..."

    cd kissat
    ./configure --quiet --no-proofs
    make
    cp build/libkissat.a .
    cd ..
fi

# Normal (non-LRAT) CaDiCaL
if echo $solvers|grep -q "c" && [ ! -f cadical/libcadical.a ]; then
    echo "Building CaDiCaL ..."

    cd cadical
    ./configure
    make
    cp build/libcadical.a .
    cd ..
fi
