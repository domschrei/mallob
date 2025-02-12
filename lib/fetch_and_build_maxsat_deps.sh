#!/bin/bash

set -e

if [ ! -d rustsat ]; then
    if [ ! -f rustsat.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="17a79f5f6d2d11dc415c943c21f238c9d984bda9"
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
    #    branchorcommit="791ab9de0169d379c41de2b76282eda405d1b9c8"
    #    wget -nc https://github.com/domschrei/maxpre-mallob/archive/${branchorcommit}.zip -O maxpre-mallob.zip
    #fi
    #unzip maxpre-mallob.zip
    #mv maxpre-mallob-* maxpre-mallob

    # still private
    git clone git@github.com:domschrei/maxpre-mallob.git
    cd maxpre-mallob
    git checkout 791ab9de0169d379c41de2b76282eda405d1b9c8
    cd ..
fi
cd maxpre-mallob
make lib with_zlib=false
cp src/lib/libmaxpre.a .
cd ..
