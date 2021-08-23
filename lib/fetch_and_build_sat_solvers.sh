#!/bin/bash

set -e

if [ -z $1 ]; then
    solvers="cglmy"
    echo "Defaulting to solvers $solvers (supply another string to override solvers to build)"
else
    solvers="$1"
fi

bash fetch_solvers.sh $solvers

if echo $solvers|grep -q "m" && [ ! -f mergesat/libmergesat.a ]; then

    # Get MergeSat (pre-patched)
    tar xzvf mergesat-patched.tar.gz

    # Make MergeSat
    cd mergesat
    make all -j $(nproc)
    cp build/release/lib/libmergesat.a .
    cd ..
fi

if echo $solvers|grep -q "g" && [ ! -f glucose/libglucose.a ]; then

    echo "Fetching Glucose ..."

    # Get Glucose and patch it
    tar xzvf glucose-syrup-4.1.tgz
    rm ._glucose-syrup-4.1
    mv glucose-syrup-4.1 glucose
    patch glucose/core/Solver.cc < Glucose_Solver.cc.patch
    
    #cp -r ../../mglucose glucose

    echo "Building Glucose ..."
    
    # Make Glucose
    cd glucose/simp
    make libr
    cp lib.a ../libglucose.a
    cd ../..
fi

if echo $solvers|grep -q "y" && [ ! -f yalsat/libyals.a ]; then

    echo "Fetching YalSAT ..."
    
    # Get YalSAT
    unzip yalsat-03v.zip
    mv yalsat-03v yalsat
    
    echo "Building YalSAT ..."
    
    cd yalsat
    ./configure.sh
    make
    cd ..
fi

if echo $solvers|grep -q "l" && [ ! -f lingeling/liblgl.a ]; then
    
    echo "Fetching lingeling ..."
    
    # Get lingeling (SAT 2018, MIT-licenced)
    tar xzvf lingeling-bcj-78ebb86-180517.tar.gz
    mv lingeling-bcj-78ebb86-180517 lingeling
    
    echo "Building lingeling ..."

    cd lingeling
    ./configure.sh
    make
    cd ..
fi

if echo $solvers|grep -q "c" && [ ! -f cadical/libcadical.a ]; then

    echo "Fetching CaDiCaL ..."

    # Get CaDiCaL
    mkdir -p cadical
    cd cadical
    tar xzvf ../cadical_clauseimport.tar.gz
    
    echo "Building CaDiCaL ..."

    ./configure
    make
    cp build/libcadical.a .
    cd ..
fi
