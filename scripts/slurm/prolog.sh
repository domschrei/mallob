#!/bin/bash

globallogdir=$MALLOB_GLOBALLOGDIR
localtmpdir=$MALLOB_LOCALTMPDIR

>&2 echo "PROLOG: $globallogdir $localtmpdir"

mkdir -p "$localtmpdir" "$globallogdir"
