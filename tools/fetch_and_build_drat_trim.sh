#!/bin/bash

# clean
rm -rf drat-trim

echo "Cloning drat-trim proof checker"
git clone https://github.com/marijnheule/drat-trim.git

echo "Making drat-trim"
cd drat-trim
sed -i 's/define MODE	1/define MODE	2/g' decompress.c
make
cd ..
