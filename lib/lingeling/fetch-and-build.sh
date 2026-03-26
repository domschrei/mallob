#!/bin/bash

source ../base-build-functions.sh
dirname="lingeling"

branchorcommit="89a167d0d2efe98d983c87b5b84175b40ea55842" # version 1.0.0, March 2024
fetch_and_extract $dirname configure.sh https://github.com/arminbiere/lingeling/archive/${branchorcommit}.zip

for f in *.c *.h ; do
    sed -i 's/exit ([01])/abort()/g' $f
done

echo "[$dirname] Building ..."
./configure.sh
make
echo "[$dirname] Build complete"
