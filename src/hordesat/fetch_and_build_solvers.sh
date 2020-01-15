#!/bin/bash

if [ ! -d minisat ]; then

    echo "Fetching minisat ..."

    # get minisat and patch it
    wget https://github.com/niklasso/minisat/archive/master.zip
    unzip master.zip
    mv minisat-master minisat
    patch minisat/minisat/core/Solver.h < minisat.h.patch
    patch minisat/minisat/core/Solver.cc < minisat.cc.patch
    patch minisat/Makefile < minisat-makefile.patch

    echo "Building minisat ..."
    
    # make minisat
    cd minisat
    make
    cd ..
else
    echo "Assuming that a correct installation of minisat is present."
fi

if [ ! -d lingeling ]; then
    
    echo "Fetching lingeling ..."
    
    # get lingeling
    
    # Option 1: Lingeling as used by original HordeSat
    wget http://fmv.jku.at/lingeling/lingeling-ayv-86bf266-140429.zip
    unzip lingeling-ayv-86bf266-140429.zip
    mv *.txt code/
    rm build.sh
    mv code lingeling
    patch lingeling/lglib.c < lingeling.patch
    
    # Option 2: SAT 2018 MIT-licenced lingeling
    #git clone https://github.com/arminbiere/lingeling.git
    
    echo "Building lingeling ..."
    
    #make lingeling
    cd lingeling
    ./configure.sh
    make
    cd ..
else
    echo "Assuming that a correct installation of lingeling is present."
fi
