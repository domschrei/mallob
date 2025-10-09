#!/bin/bash

#SWEEP WITHIN SATWITHPRE !

NPROCS=4
threads=3
echo "NPROCS $NPROCS"
echo "threads per process $threads"

OUT_DIR=$HOME/PhD/logsntraces/
INST_DIR=$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf

#-preprocess-sweep \

MALLOB_OPTIONS="-t=$threads \
  -mono-app=SATWITHPRE \
  -sweep-sharing-period=50 \
  -satsolver=k \
  -colors \
  -v=4 \
  -jcup=0.05 \
  -trace-dir=$OUT_DIR/traces/ \
  -log=$OUT_DIR/logs/ \
  -mono=$INST_DIR"


#clean old logs and traces
rm -rf $HOME/PhD/logsntraces/logs/*
rm -rf $HOME/PhD/logsntraces/traces/*

RDMAV_FORK_SAFE=1; 

# SAT Valgrind subprocess
# MALLOB_OPTIONS="$MALLOB_OPTIONS -subproc-prefix=scripts/run/run_as_valgrind.sh"

echo "MALLOB_OPTIONS"
echo $MALLOB_OPTIONS | tr ' ' '\n'

mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} build/mallob $MALLOB_OPTIONS

#MPI Valgrind
# mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} valgrind --leak-check=full build/mallob $MALLOB_OPTIONS

# 
