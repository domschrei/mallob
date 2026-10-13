#!/bin/bash

./scripts/sweeping/fetch-local-sweep-kissat.sh

./scripts/setup/cmake-make.sh build -DMALLOB_APP_{SMT,MAXSAT,INCSAT,PALRUPCHECK,SATWITHPRE,SWEEP}=0 -DMALLOB_LOG_VERBOSITY=4 -DMALLOB_UNIT_TESTS=0

