#!/bin/bash

# Fetch Marijn's preprocessor tool as needed
if [ ! -d ../../tools/preprocess-simple ]; then
    ( cd ../../tools && git clone git@github.com:marijnheule/preprocess-simple.git )
fi

docker build -t mallob:leader ../.. -f ./Dockerfile

