#!/bin/bash

set -e

if [ -z "$1" ]; then echo "Usage: bash scripts/setup/cmake-make.sh <build-dir> [build-opts]"; exit 1; fi

builddir="$1"
shift 1

mkdir -p "$builddir"
priordir=$(pwd)
cd "$builddir"

cmake -DMALLOB_SUBPROC_DISPATCH_PATH=\""$builddir"/\" $@ ..

#VERBOSE=1 \
make -j

cd "$priordir"
