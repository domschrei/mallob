#!/bin/bash

source ../base-build-functions.sh
dirname="jemalloc"

branchorcommit="e36a0fa5bc1e1090362505ac4af4408466ba5163" # updated 2026-08-13
fetch_and_extract $dirname autogen.sh https://github.com/jemalloc/jemalloc/archive/${branchorcommit}.zip
echo "[jemalloc] Building ..."
./autogen.sh
make
rm lib/*.so*
echo "[jemalloc] Build complete"
