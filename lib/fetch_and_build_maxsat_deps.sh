#!/bin/bash

set -e

if [ ! -d rustsat ]; then
    if [ ! -f rustsat.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="58bdca09d345105ca25bd8894d98915cc17c13aa"
        wget -nc https://github.com/domschrei/rustsat/archive/${branchorcommit}.zip -O rustsat.zip
    fi
    unzip rustsat.zip
    mv rustsat-* rustsat
fi
cd rustsat/capi
cargo build --release
cd ..
cp target/release/librustsat_capi.a librustsat.a
cd ..

if [ ! -d maxpre-mallob ]; then
    # once public
    #if [ ! -f maxpre-mallob.zip ]; then
    #    # for fixing a branch instead of a commit, prepend "refs/heads/"
    #    branchorcommit="d17de9afb0094e6b4c45308eb519961a2e6cd0b1"
    #    wget -nc https://github.com/domschrei/maxpre-mallob/archive/${branchorcommit}.zip -O maxpre-mallob.zip
    #fi
    #unzip maxpre-mallob.zip
    #mv maxpre-mallob-* maxpre-mallob

    # still private
    git clone git@github.com:domschrei/maxpre-mallob.git
    cd maxpre-mallob
    git checkout d17de9afb0094e6b4c45308eb519961a2e6cd0b1
    cd ..
fi
cd maxpre-mallob
make lib with_zlib=false
cp src/lib/libmaxpre.a .
cd ..
