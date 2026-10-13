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
    # disable all other kinds of libraries, dependencies and add-ons (except for Kissat)
    frontopts="${frontopts}-DMALLOB_BUILD_{IMPCHECK,CHECKER,CHAINCHECK}=0 \
    -DMALLOB_USE_{ASAN,JEMALLOC,MINISAT,CADICAL,LINGELING,RUSTSAT,MAXPRE,SATSUMA}=0"
fi

cd "$builddir"

cmake $(eval echo $frontopts) -DMALLOB_SUBPROC_DISPATCH_PATH=\""$builddir"/\" -DCMAKE_BUILD_TYPE=RELEASE $@ .. \
     || ( printf "\n\
An error occurred during the configuration stage of building.\n\
Please check that all required system dependencies are installed on your system:\n\
  https://github.com/domschrei/mallob/tree/master/docs/setup.md\n\
(or locally: docs/setup.md).\n\n" && false )

#VERBOSE=1 \
make -j || ( printf "\n\
An error occurred during the compilation or linking stage of building.\n\
Please check that all required system dependencies are installed on your system:\n\
  https://github.com/domschrei/mallob/tree/master/docs/setup.md\n\
(or locally: docs/setup.md).\n\n" && false )

cd "$priordir"
