#!/bin/bash

if false; then
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
fi

if [ ! -d yalsat ]; then

    # Get YalSAT
    echo "Fetching YalSAT ..."
    wget http://fmv.jku.at/yalsat/yalsat-03v.zip
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

if [ ! -d lingeling ]; then
    
    # get lingeling
    echo "Fetching lingeling ..."
    
    # Option 1: Lingeling as used by original HordeSat
    #wget http://fmv.jku.at/lingeling/lingeling-ayv-86bf266-140429.zip
    #unzip lingeling-ayv-86bf266-140429.zip
    #mv *.txt code/
    #rm build.sh
    #mv code lingeling
    
    # Option 2: SAT 2018 MIT-licenced lingeling
    wget http://fmv.jku.at/lingeling/lingeling-bcj-78ebb86-180517.tar.gz
    tar xzvf lingeling-bcj-78ebb86-180517.tar.gz
    mv lingeling-bcj-78ebb86-180517 lingeling
    
    echo "Building lingeling ..."
    #patch lingeling/lglib.c < lingeling.patch
    cd lingeling
    ./configure.sh
    make
    cd ..
else
    echo "Assuming that a correct installation of lingeling is present."
fi

if [ ! -d cadical ]; then

    # get cadical
    echo "Fetching CaDiCaL ..."
    git clone https://github.com/arminbiere/cadical
    cd cadical
    ./configure
    make
    cp build/libcadical.a .
    cd ..

else
    echo "Assuming that a correct installation of cadical is present."
fi