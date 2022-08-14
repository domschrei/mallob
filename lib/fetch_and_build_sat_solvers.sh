#!/bin/bash

set -e

if [ -z $1 ]; then
    solvers="c"
    echo "Defaulting to solvers $solvers (supply another string to override solvers to build)"
else
    solvers="$1"
fi

bash fetch_solvers.sh $solvers

echo "Building frat-rs (for proof checking) ..."
export PATH="/root/.cargo/bin:$PATH"
cd frat
make
cd ..
echo "*****  looking for frat!"
find / -name "frat*"
find / -name "frat-rs"


if echo $solvers|grep -q "m" && [ ! -f mergesat/libmergesat.a ]; then
    echo "Building MergeSAT"

    tar xzvf mergesat-patched.tar.gz
    cd mergesat
    make all -j $(nproc)
    cp build/release/lib/libmergesat.a .
    cd ..
fi

if echo $solvers|grep -q "g" && [ ! -f glucose/libglucose.a ]; then
    echo "Building Glucose ..."
    
    tar xzvf glucose-syrup-4.1.tgz
    rm ._glucose-syrup-4.1
    mv glucose-syrup-4.1 glucose
    # Patch bug in solver
    patch glucose/core/Solver.cc < Glucose_Solver.cc.patch
    # Add INCREMENTAL definition to compile flags
    sed -i 's/ -D __STDC_FORMAT_MACROS/ -D __STDC_FORMAT_MACROS -D INCREMENTAL/g' glucose/mtl/template.mk
    # Fix typo in a preprocessor definition
    sed -i 's/INCREMNENTAL/INCREMENTAL/g' glucose/core/SolverTypes.h
    
    cd glucose/simp
    make libr
    cp lib.a ../libglucose.a
    cd ../..
fi

if echo $solvers|grep -q "y" && [ ! -f yalsat/libyals.a ]; then
    echo "Building YalSAT ..."

    unzip yalsat-03v.zip
    mv yalsat-03v yalsat
    cd yalsat
    for f in *.c *.h ; do
        sed -i 's/exit ([01])/abort()/g' $f
    done
    ./configure.sh
    make
    cd ..
fi

if echo $solvers|grep -q "l" && [ ! -f lingeling/liblgl.a ]; then
    echo "Building lingeling ..."

    #tar xzvf lingeling-bcj-78ebb86-180517.tar.gz
    #mv lingeling-bcj-78ebb86-180517 lingeling
    unzip lingeling-isc22.zip
    mv lingeling-*/ lingeling
    cd lingeling
    for f in *.c *.h ; do
        sed -i 's/exit ([01])/abort()/g' $f
    done
    ./configure.sh
    make
    cd ..
fi

if echo $solvers|grep -q "c" && [ ! -f cadical/libcadical.a ]; then
    echo "Building CaDiCaL ..."

    cd cadical
    ./configure
    make
    cp build/libcadical.a .
    cd ..
fi

if echo $solvers|grep -q "k" && [ ! -f kissat/libkissat.a ]; then
    echo "Building kissat ..."

    unzip kissat-isc22.zip
    mv kissat-*/ kissat
    cd kissat
    ./configure --quiet
    make
    cp build/libkissat.a .
    cd ..
fi
