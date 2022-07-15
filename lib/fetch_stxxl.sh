#!/bin/bash

git clone https://github.com/stxxl/stxxl.git
cd stxxl
git checkout b9e44f0ecba7d7111fbb33f3330c3e53f2b75236

# These commands are not strictly necessary (STXXL is built automatically)
# but they create a header file which IDEs expect during development.
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cd ..
cp build/include/stxxl/bits/config.h include/stxxl/bits/config.h
