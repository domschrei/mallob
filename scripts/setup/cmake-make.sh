#!/bin/bash

set -e

if [ -z "$1" ]; then echo "Usage: bash scripts/setup/cmake-make.sh <build-dir> [build-opts]"; exit 1; fi

builddir="$1"
shift 1

mkdir -p "$builddir"
priordir=$(pwd)

frontopts=""
if [ "x$MALLOB_MINIMAL" == "x1" ]; then
    # disable ALL applications except for SAT
    for d in src/app/*/ ; do
        if [ "$d" == "src/app/sat/" ]; then continue; fi
        frontopts="${frontopts}-DMALLOB_APP_$(basename $d | tr '[:lower:]' '[:upper:]')=0 "
    done
    # disable all other kinds of optional add-ons
    frontopts="${frontopts}-DMALLOB_BUILD_{IMPCHECK,CHECKER,CHAINCHECK}=0 \
    -DMALLOB_USE_{ASAN,JEMALLOC,MINISAT,CADICAL,LINGELING,RUSTSAT,MAXPRE,SATSUMA}=0"
fi

cd "$builddir"

cmake $(eval echo $frontopts) -DMALLOB_SUBPROC_DISPATCH_PATH=\""$builddir"/\" -DCMAKE_BUILD_TYPE=RELEASE $@ ..

#VERBOSE=1 \
make -j

cd "$priordir"
