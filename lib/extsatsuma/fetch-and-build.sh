#!/bin/bash

source ../base-build-functions.sh
dirname="satsuma-dev-sc26"

# For now, expect a bundled Satsuma ZIP in this directory:
unzip ${dirname}*.zip
for d in $dirname-*/ ; do
    cd "$d"
    mv $(find . -maxdepth 1 -mindepth 1) ../
    cd ..
done
rmdir $dirname-*/
# Once Satsuma version is public, instead do something like this:
#branchorcommit="xxxxxxxx"
#fetch_and_extract $dirname CMakeLists.txt https://github.com/xxx/yyy/archive/${branchorcommit}.zip

# Disable unlocked file I/O since this may cause trouble with pipes
sed -i 's/#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))/#if false/g' src/utility.h

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=RELEASE ..
make -j
cd ..
echo "[$dirname] Build complete"

if ! [ -z "$1" ]; then
    echo "[$dirname] cp build/satsuma $1/"
    cp build/satsuma "$1/"
fi
