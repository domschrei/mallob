#!/bin/bash


NPROCS=4
threads=1
echo "NPROCS $NPROCS"
echo "threads per process $threads"

OUT_DIR=$HOME/PhD/logsntraces/
INST_DIR=$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf

MALLOB_OPTIONS=" \
  -t=$threads \
  -mono-app=SWEEP \
  -satsolver=k \
  -colors \
  -v=2 \
  -jcup=0.05 \
  -trace-dir=$OUT_DIR/traces/ \
  -log=$OUT_DIR/logs/ \
  -mono=$INST_DIR"

#clean old logs and traces
rm -rf $HOME/PhD/logsntraces/logs/*
rm -rf $HOME/PhD/logsntraces/traces/*

RDMAV_FORK_SAFE=1; 
mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} build/mallob $MALLOB_OPTIONS
