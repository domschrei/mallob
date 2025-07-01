#!/bin/bash

KISSAT_SWEEP=$HOME/PhD/ksst-sweep/kissat/
MALLOB_LIB=$HOME/PhD/mallob/lib/

if cmp --silent -- "$KISSAT_SWEEP/build/libkissat.a" "$MALLOB_LIB/kissat/libkissat.a"; then
  echo "libkissat.a no change, no update"
else
  echo "libkissat.a changed. Updated libkissat.a from ksst-sweep/kissat/"
  cp -r $KISSAT_SWEEP/src $MALLOB_LIB/kissat
  cp $KISSAT_SWEEP/build/libkissat.a $MALLOB_LIB/kissat/libkissat.a
fi
