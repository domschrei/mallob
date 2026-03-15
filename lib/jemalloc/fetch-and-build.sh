#!/bin/bash

set -e

if [ ! -f autogen.sh ]; then
    if [ ! -f jemalloc.zip ]; then
        echo "[jemalloc] Fetching sources ..."
        curl -L -o jemalloc.zip https://github.com/jemalloc/jemalloc/archive/refs/tags/5.2.1.zip
    fi
    echo "[jemalloc] Extracting sources ..."
    unzip jemalloc.zip
    mv jemalloc-*/* jemalloc-*/.* ./
    rmdir jemalloc-*/
else
    echo "[jemalloc] Assuming sources are present"
fi

echo "[jemalloc] Building ..."
./autogen.sh
make
echo "[jemalloc] Build complete"
