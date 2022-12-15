#!/bin/bash

set -x


# 1. proof composition tools

cd composition

# clean
rm -rf build || true

echo "Building proof composition tools"
make
cd ..


# 2. drat-trim

# clean
rm -rf drat-trim || true

echo "Cloning drat-trim proof checker"
git clone https://github.com/marijnheule/drat-trim.git

echo "Building drat-trim"
cd drat-trim
sed -i 's/define MODE	1/define MODE	2/g' decompress.c
make
cd ..


# 3. preprocessor

# clean
rm -rf preprocess-simple || true

echo "Cloning certified preprocessor"
git clone https://github.com/marijnheule/preprocess-simple.git

echo "Building certified preprocessor"
cd preprocess-simple
make
cd ..
