#!/bin/bash

set -e

if [ ! -f mergesat/libmergesat.a ]; then

    # Get MergeSat and patch it
    git clone https://github.com/conp-solutions/mergesat.git
    cd mergesat
    git checkout devel # might be a specific commit lateron
    
    # Change include paths in files in order not to collide with Glucose's includes
    set +e
    for d in core mtl simp utils ; do
        for f in $d/*.cc $d/*.h ; do
            sed -i 's,#include "core/,#include "mergesat/minisat/core/,g' $f
            sed -i 's,#include "mtl/,#include "mergesat/minisat/mtl/,g' $f 
            sed -i 's,#include "simp/,#include "mergesat/minisat/simp/,g' $f
            sed -i 's,#include "utils/,#include "mergesat/minisat/utils/,g' $f
        done
    done
    sed -i 's,MINISAT_CXXFLAGS = -I. ,MINISAT_CXXFLAGS = -I.. -I. ,g' Makefile
    set -e

    # Fix FPE bug in solver for particular diversifications 
    sed -i 's,new_activity = 1000 / v;,if (v != 0) new_activity = 1000 / v;,g' core/Solver.cc
    
    cd ..

    # fixup MergeSat to provide hook
    # patch minisat/minisat/core/Solver.h < minisat.h.patch
    # patch minisat/minisat/core/Solver.cc < minisat.cc.patch

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
    wget www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
    tar xzvf glucose-syrup-4.1.tgz
    rm glucose-syrup-4.1.tgz ._glucose-syrup-4.1
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
    wget http://fmv.jku.at/yalsat/yalsat-03v.zip
    unzip yalsat-03v.zip
    rm yalsat-03v.zip
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
    wget http://fmv.jku.at/lingeling/lingeling-bcj-78ebb86-180517.tar.gz
    tar xzvf lingeling-bcj-78ebb86-180517.tar.gz
    rm lingeling-bcj-78ebb86-180517.tar.gz
    mv lingeling-bcj-78ebb86-180517 lingeling
    
    echo "Building lingeling ..."

    cd lingeling
    ./configure.sh
    make
    cd ..
else
    echo "Assuming that a correct installation of lingeling is present."
fi

if [ ! -f cadical/libcadical.a ]; then

    echo "Fetching CaDiCaL ..."

    # Get CaDiCaL
    wget https://dominikschreiber.de/cadical_clauseimport.tar.gz
    mkdir -p cadical
    cd cadical
    tar xzvf ../cadical_clauseimport.tar.gz
    rm ../cadical_clauseimport.tar.gz
    
    echo "Building CaDiCaL ..."

    ./configure
    make
    cp build/libcadical.a .
    cd ..

else
    echo "Assuming that a correct installation of cadical is present."
fi
