#!/bin/bash

./scripts/sweeping/fetch-local-sweep-kissat.sh

./scripts/setup/cmake-make.sh build -DMALLOB_APP_{SMT,MAXSAT,INCSAT,PALRUPCHECK}=0 

