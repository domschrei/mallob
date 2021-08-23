#!/bin/bash

set -e

bash fetch_solvers.sh

if [ ! -f mergesat/libmergesat.a ]; then

    # Get MergeSat (pre-patched)
    tar xzvf mergesat-patched.tar.gz

    # Make MergeSat
    cd mergesat
    make all -j $(nproc)
    cp build/release/lib/libmergesat.a .
    cd ..
else
    echo "Assuming that a correct installation of MergeSat is present."
fi

if [ ! -f glucose/libglucose.a ]; then

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
    make rs libr
    cp lib.a ../libglucose.a
    cd ../..
else
    echo "Assuming that a correct installation of Glucose is present."
fi

if [ ! -f yalsat/libyals.a ]; then

    echo "Fetching YalSAT ..."
    
    # Get YalSAT
    unzip yalsat-03v.zip
    mv yalsat-03v yalsat
    
    echo "Building YalSAT ..."
    
    cd yalsat
    ./configure.sh
    make
    cd ..
else
    echo "Assuming that a correct installation of YalSAT is present."
fi

if [ ! -f lingeling/liblgl.a ]; then
    
    echo "Fetching lingeling ..."
    
    # Get lingeling (SAT 2018, MIT-licenced)
    tar xzvf lingeling-bcj-78ebb86-180517.tar.gz
    mv lingeling-bcj-78ebb86-180517 lingeling
    
    echo "Building lingeling ..."

    cd lingeling
    ./configure.sh
    make
    cd ..
else
    echo "Assuming that a correct installation of Lingeling is present."
fi

if [ ! -f cadical/libcadical.a ]; then

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

else
    echo "Assuming that a correct installation of CaDiCaL is present."
fi
