#!/bin/bash

set -e

if [ ! -d rustsat ]; then
    if [ ! -f rustsat.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="f78f9d7a007a00538253dc376696dd39a06d0498"
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
    #    branchorcommit="ee8527d080acac06bb6756f5fa2c1c2f77808595"
    #    wget -nc https://github.com/domschrei/maxpre-mallob/archive/${branchorcommit}.zip -O maxpre-mallob.zip
    #fi
    #unzip maxpre-mallob.zip
    #mv maxpre-mallob-* maxpre-mallob

    # still private
    git clone git@github.com:domschrei/maxpre-mallob.git
    cd maxpre-mallob
    git checkout ee8527d080acac06bb6756f5fa2c1c2f77808595
    cd ..
fi
cd maxpre-mallob
make lib with_zlib=false
cp src/lib/libmaxpre.a .
cd ..
