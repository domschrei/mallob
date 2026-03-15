#!/bin/bash

set -e

if [ ! -f configure ]; then
    echo "Fetching solver sources ..."
    if [ ! -f cadical.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="be7a0f84190b3216c589696b2010e8cbf8a8252e" # updated 2026-02-05
        curl -L -o cadical.zip https://github.com/domschrei/cadical/archive/${branchorcommit}.zip
        #wget -nc https://github.com/domschrei/cadical/archive/${branchorcommit}.zip -O cadical.zip
    fi
    echo "Extracting solver sources ..."
    unzip cadical.zip
    mv cadical-*/* cadical-*/.* ./
    rmdir cadical-*/
else
    echo "Assuming solver sources are present"
fi

echo "Building"
./configure
make -j
echo "Solver built"
