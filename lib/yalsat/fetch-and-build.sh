#!/bin/bash

source ../base-build-functions.sh
dirname="yalsat"

fetch_and_extract $dirname configure.sh http://fmv.jku.at/yalsat/yalsat-03v.zip

disable_fpu=false

for f in *.c *.h ; do
    sed -i 's/exit ([01])/abort()/g' $f
done
if $disable_fpu; then
    sed -i 's/#ifdef __linux__/#if 0/g' yals.c
fi

echo "[$dirname] Building ..."
./configure.sh
make
echo "[$dirname] Build complete"
