#!/bin/bash

set -e

if [ ! -f CMakeLists.txt ]; then
    if [ ! -f palrup.zip ]; then
        echo "[palrup] Fetching sources ..."
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="3d41651b8af222305af6af0d757936231dc88783" # updated 2026-01-29
        curl -L -o palrup.zip https://github.com/rubenGoetz/impcheck/archive/${branchorcommit}.zip
    fi
    echo "[palrup] Extracting sources ..."
    unzip palrup.zip
    mv impcheck-*/* impcheck-*/.* ./ || :
    rmdir impcheck-*/
    sed -i 's/-Werror//g' CMakeLists.txt
else
    echo "[palrup] Assuming sources are present"
fi

echo "[palrup] Building ..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DIMPCHECK_PLRAT=1 -DIMPCHECK_WRITE_DIRECTIVES=0 -DIMPCHECK_FLUSH_ALWAYS=0
make
cd ..
echo "[palrup] Build complete"
