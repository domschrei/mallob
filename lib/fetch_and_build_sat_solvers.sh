#!/bin/bash

set -e

if [ ! -f glucose/libglucose.a ]; then

    echo "Fetching Glucose ..."

    # Get Glucose and patch it
    wget www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
    tar xzvf glucose-syrup-4.1.tgz
    rm glucose-syrup-4.1.tgz ._glucose-syrup-4.1
    mv glucose-syrup-4.1 glucose
    patch glucose/core/Solver.cc < Glucose_Solver.cc.patch
    
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
    git clone https://github.com/arminbiere/cadical
    
    echo "Building CaDiCaL ..."

    cd cadical
    ./configure
    make
    cp build/libcadical.a .
    cd ..

else
    echo "Assuming that a correct installation of cadical is present."
fi
