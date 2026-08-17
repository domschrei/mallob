#!/bin/bash

set -e

if [ -z "$1" ]; then echo "Usage: bash scripts/setup/cmake-make.sh <build-dir> [build-opts]"; exit 1; fi

builddir="$1"
shift 1

mkdir -p "$builddir"
priordir=$(pwd)
cd "$builddir"

frontopts=""
if [ "x$MALLOB_MINIMAL" == "x1" ]; then
    frontopts="-DMALLOB_APP_{INCSAT,KMEANS,MAXSAT,SMT,PALRUPCHECK,SATCNC,SATWITHPRE}=0 \
    -DMALLOB_BUILD_{IMPCHECK,CHECKER}=0 \
    -DMALLOB_USE_{ASAN,JEMALLOC,MINISAT,CADICAL,LINGELING,KISSAT,RUSTSAT,MAXPRE,SATSUMA}=0"
fi

cmake $(eval echo $frontopts) -DMALLOB_SUBPROC_DISPATCH_PATH=\""$builddir"/\" -DCMAKE_BUILD_TYPE=RELEASE $@ ..

#VERBOSE=1 \
make -j

cd "$priordir"
