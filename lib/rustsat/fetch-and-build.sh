#!/bin/bash

source ../base-build-functions.sh
dirname="rustsat"

branchorcommit="17a79f5f6d2d11dc415c943c21f238c9d984bda9"
fetch_and_extract $dirname Cargo.toml https://github.com/domschrei/rustsat/archive/${branchorcommit}.zip

echo "[$dirname] Building ..."
cd capi
cargo build --release
cd ..
echo "[$dirname] Build complete"
