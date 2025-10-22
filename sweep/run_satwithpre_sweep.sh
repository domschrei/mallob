#!/bin/bash

NPROCS=4
THREADS=3

OUT_DIR="$HOME/PhD/logsntraces/"
INST_PATH="$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf"

SWEEP=true
SWEEP_PRIO=1.0
PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE=false

# --- Argument parsing ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sweep)
      SWEEP=false
      shift
      ;;
    --sweep-prio=*)
      SWEEP_PRIO="${1#*=}"
      shift
      ;;
    -pssc)
      PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE=true
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--no-sweep] [--sweep-prio=<float>] [-pssc]"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

#priority 0 -->deactivate sweeping
if [[ "$SWEEP_PRIO" =~ ^0*\.?0*$ ]]; then 
  SWEEP=false
fi

#do single solver sequential sweeping instead of the shared sweeping
if $PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE; then
  SWEEP=false
fi

echo "NPROCS $NPROCS"
echo "Threads per process $THREADS"
echo "SWEEP priority $SWEEP_PRIO"
echo "SWEEP: $SWEEP"
echo "PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE: $PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE"



MALLOB_OPTIONS="-t=$THREADS \
  -mono-app=SATWITHPRE \
  -v=4 \
  -satsolver=[k_]w \
  -colors \
  -jcup=0.05 \
  -trace-dir=$OUT_DIR/traces/ \
  -log=$OUT_DIR/logs/ \
  -mono=$INST_PATH \
  -sweep-sharing-period=20 \
  -sweep-solver-verbosity=1"

if $SWEEP; then
    echo "SWEEP Preprocessing ADDED !"
    MALLOB_OPTIONS="$MALLOB_OPTIONS -preprocess-sweep -preprocess-sweep-priority=$SWEEP_PRIO"
else
    echo "SWEEP Preprocessing SKIPPED !"
fi

if $PREPROCESS_SEQUENTIAL_SWEEPCOMPLETE; then
    echo "Preprocess sequential sweepcomplete ADDED"
    MALLOB_OPTIONS="$MALLOB_OPTIONS -pssc"
fi

#clean old logs and traces
rm -rf "$HOME/PhD/logsntraces/logs"/*
rm -rf "$HOME/PhD/logsntraces/traces"/*

RDMAV_FORK_SAFE=1; 

# SAT Valgrind subprocess
# MALLOB_OPTIONS="$MALLOB_OPTIONS -subproc-prefix=scripts/run/run_as_valgrind.sh"

echo "MALLOB_OPTIONS"
echo $MALLOB_OPTIONS | tr ' ' '\n'

mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${THREADS} build/mallob $MALLOB_OPTIONS

#MPI Valgrind
# mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} valgrind --leak-check=full build/mallob $MALLOB_OPTIONS

