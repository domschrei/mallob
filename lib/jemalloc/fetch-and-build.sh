#!/bin/bash

source ../base-build-functions.sh
dirname="jemalloc"

fetch_and_extract $dirname autogen.sh https://github.com/jemalloc/jemalloc/archive/refs/tags/5.2.1.zip

echo "[jemalloc] Building ..."
./autogen.sh
make
rm lib/jemalloc*.so
echo "[jemalloc] Build complete"
