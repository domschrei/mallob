#!/bin/bash

set -e

if [ ! -f autogen.sh ]; then
    if [ ! -f jemalloc.zip ]; then
        echo "Fetching sources ..."
        curl -L -o jemalloc.zip https://github.com/jemalloc/jemalloc/archive/refs/tags/5.2.1.zip
    fi
    echo "Extracting sources ..."
    unzip jemalloc.zip
    mv jemalloc-*/* jemalloc-*/.* ./
    rmdir jemalloc-*/
else
    echo "Assuming sources are present"
fi

echo "Building"
./autogen.sh
make
echo "jemalloc built"
