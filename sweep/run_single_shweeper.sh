#!/bin/bash

INST_DIR="$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf"

OPTS="--mallob_is_shweeper=1 \
  --sweepcomplete=1 \
  --probe=1 \
  --verbose=1 \
  --mallob_custom_sweep_verbosity=2 \
  --check=0 \
  $INST_DIR"

# ~/PhD/ksst-sweep/kissat/build/kissat $OPTS
# valgrind ~/PhD/ksst-sweep/kissat/build/kissat $OPTS
valgrind --leak-check=full ~/PhD/ksst-sweep/kissat/build/kissat $OPTS
