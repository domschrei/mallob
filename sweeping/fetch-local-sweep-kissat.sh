#!/bin/bash

echo "Copying local sweep-kissat into mallob lib"
# ( cd lib && bash fetch_and_build_solvers.sh kcly )
KISSAT_SWEEP=$HOME/PhD/ksst-sweep/kissat/
MALLOB_LIB=$HOME/PhD/mallob/lib/
cp -r $KISSAT_SWEEP/src $MALLOB_LIB/kissat
cp $KISSAT_SWEEP/build/libkissat.a $MALLOB_LIB/kissat/libkissat.a
