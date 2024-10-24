#!/bin/bash

if [ ! -d rustsat ]; then
    git clone git@github.com:domschrei/rustsat.git
    cd rustsat
    git checkout next-major
    cd ..
fi
cd rustsat/capi
cargo build --release
cd ..
cp target/release/librustsat_capi.a librustsat.a
cd ..

if [ ! -d maxpre-mallob ]; then
    git clone git@github.com:domschrei/maxpre-mallob.git
    cd maxpre-mallob
    git checkout 542e9a0be4630e1d8b628ee38ef05334e6bfe503
    cd ..
fi
cd maxpre-mallob
make lib with_zlib=false
cp src/lib/libmaxpre.a .
cd ..
