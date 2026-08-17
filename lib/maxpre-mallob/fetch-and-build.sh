#!/bin/bash

source ../base-build-functions.sh
dirname="maxpre-mallob"

branchorcommit="ccd759aab16dae3d8e74f021d0955d17c424ed38"
fetch_and_extract $dirname Makefile https://github.com/jezberg/maxpre-mallob/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
make lib with_zlib=false
echo "[$dirname] Build complete"
