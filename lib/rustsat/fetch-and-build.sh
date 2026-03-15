#!/bin/bash

set -e

if [ ! -f Cargo.toml ]; then
    echo "[rustsat] Fetching sources ..."
    if [ ! -f rustsat.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
        branchorcommit="17a79f5f6d2d11dc415c943c21f238c9d984bda9"
        wget -nc https://github.com/domschrei/rustsat/archive/${branchorcommit}.zip -O rustsat.zip
    fi
    echo "[rustsat] Extracting sources ..."
    unzip rustsat.zip
    mv rustsat-*/* rustsat-*/.* ./
    rmdir rustsat-*/
else
    echo "[rustsat] Assuming sources are present"
fi

echo "[rustsat] Building ..."
cd capi
cargo build --release
cd ..
echo "[rustsat] Build complete"
