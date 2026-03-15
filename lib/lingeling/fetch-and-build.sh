#!/bin/bash

set -e

if [ ! -f configure.sh ]; then
    if [ ! -f lingeling.zip ]; then
        echo "Fetching solver sources ..."
        branchorcommit="89a167d0d2efe98d983c87b5b84175b40ea55842" # version 1.0.0, March 2024
        curl -L -o lingeling.zip https://github.com/arminbiere/lingeling/archive/${branchorcommit}.zip
    fi
    echo "Extracting solver sources ..."
    unzip lingeling.zip
    mv lingeling-*/* lingeling-*/.* ./
    rmdir lingeling-*/
else
    echo "Assuming solver sources are present"
fi

echo "Building"
for f in *.c *.h ; do
    sed -i 's/exit ([01])/abort()/g' $f
done
./configure.sh
make
echo "Solver built"
