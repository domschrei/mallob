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
