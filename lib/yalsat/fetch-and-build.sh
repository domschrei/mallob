#!/bin/bash

set -e
disable_fpu=false

if [ ! -f configure.sh ]; then
    if [ ! -f yalsat-*.zip ]; then
        echo "[lingeling:yalsat] Fetching sources ..."
        curl -L -o yalsat-03v.zip -C - http://fmv.jku.at/yalsat/yalsat-03v.zip
    fi
    echo "[lingeling:yalsat] Extracting sources ..."
    unzip yalsat-03v.zip
    mv yalsat-*/* yalsat-*/.* ./
    rmdir yalsat-*/

    for f in *.c *.h ; do
        sed -i 's/exit ([01])/abort()/g' $f
    done
    if $disable_fpu; then
        sed -i 's/#ifdef __linux__/#if 0/g' yals.c
    fi
    
    ./configure.sh
else
    echo "[lingeling:yalsat] Assuming sources are present"
fi

echo "[lingeling:yalsat] Building ..."
make
echo "[lingeling:yalsat] Build complete"
