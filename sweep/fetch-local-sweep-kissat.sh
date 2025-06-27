#!/bin/bash

KISSAT_SWEEP=$HOME/PhD/ksst-sweep/kissat/
MALLOB_LIB=$HOME/PhD/mallob/lib/

if cmp --silent -- "$KISSAT_SWEEP/build/libkissat.a" "$MALLOB_LIB/kissat/libkissat.a"; then
  echo "Mallob lib: libkissat.a is already up to date. No Change"
else
  echo "Mallob lib: libkissat.a via local source updated"
  cp -r $KISSAT_SWEEP/src $MALLOB_LIB/kissat
  cp $KISSAT_SWEEP/build/libkissat.a $MALLOB_LIB/kissat/libkissat.a
fi
