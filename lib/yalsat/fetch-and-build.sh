#!/bin/bash

set -e
disable_fpu=false

if [ ! -f configure.sh ]; then
    if [ ! -f yalsat-*.zip ]; then
        echo "Fetching solver sources ..."
        curl -L -o yalsat-03v.zip -C - http://fmv.jku.at/yalsat/yalsat-03v.zip
    fi
    echo "Extracting solver sources ..."
    unzip yalsat-03v.zip
    mv yalsat-*/* yalsat-*/.* ./
    rmdir yalsat-*/
else
    echo "Assuming solver sources are present"
fi

echo "Building"
for f in *.c *.h ; do
    sed -i 's/exit ([01])/abort()/g' $f
done
if $disable_fpu; then
    sed -i 's/#ifdef __linux__/#if 0/g' yals.c
fi
./configure.sh
make
cd ..
echo "Solver built"
