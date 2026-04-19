#!/bin/bash

source ../base-build-functions.sh
dirname="rustsat"

branchorcommit="17a79f5f6d2d11dc415c943c21f238c9d984bda9"
fetch_and_extract $dirname Cargo.toml https://github.com/domschrei/rustsat/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
cd capi
if [ ! -d vendor ]; then
    cargo vendor
    mkdir -p .cargo
    echo '[source.crates-io]'                   >> .cargo/config.toml
    echo 'replace-with = "vendored-sources"'    >> .cargo/config.toml
    echo ''                                     >> .cargo/config.toml
    echo '[source.vendored-sources]'            >> .cargo/config.toml
    echo 'directory = "vendor"'                 >> .cargo/config.toml
fi
cargo build --release
cd ..
echo "[$dirname] Build complete"
