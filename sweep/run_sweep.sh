#!/bin/bash


NPROCS=2
threads=2
echo "NPROCS $NPROCS"
echo "threads per process $threads"

OUT_DIR=$HOME/PhD/logsntraces/

# INST_PATH=$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/nusmvdme1d3multi.cnf.xz" #0.1sec
INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/beemndhm2b2.cnf.xz" # 6sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/iso/6s151.cnf.xz"  # 0.1sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/bob12s01.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/cmudme1.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/some2024/39fba35826ce8c87cd8e8de1969b2dd2-SGI_30_80_26_70_4-log.shuffled-as.sat03-208.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/some2024/39277cab188349aee0f229cb7341b5c5-crafted_n12_d6_c4_num23.cnf.xz"


MALLOB_OPTIONS="-t=$threads \
  -mono-app=SWEEP \
  -satsolver=k \
  -colors \
  -trace-dir=$OUT_DIR/traces/ \
  -log=$OUT_DIR/logs/ \
  -mono=$INST_PATH \
  -os=1 \
  -iff=0 \
  -cm=0 \
  -rspaa=1 \
	-rpa=1 \
	-seed=1 \
	-v=2 \
  -spl=4 \
	-jcup=0.05 \
	-preprocess-sweep=1 \
	-sweep-sharing-period=0.020 \
  -sweep-resweep-chance=1000 \
  -sweep-iterations=2 \
	-sweep-solver-verbosity=2 \
	-sweep-solver-quiet=1 \
  -sweep-congruence=0 \
  -sweep-max-growth-iteration=3 \
  -sweep-max-empty-rounds=5 \
  -preprocess-sweep-priority=1.0 \
"


#clean old logs and traces
rm -rf $HOME/PhD/logsntraces/logs/*
rm -rf $HOME/PhD/logsntraces/traces/*

RDMAV_FORK_SAFE=1; 

# SAT Valgrind subprocess
# MALLOB_OPTIONS="$MALLOB_OPTIONS -subproc-prefix=scripts/run/run_as_valgrind.sh"

echo $MALLOB_OPTIONS | tr ' ' '\n'

mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} build/mallob $MALLOB_OPTIONS

#MPI Valgrind
# mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} valgrind --leak-check=full build/mallob $MALLOB_OPTIONS

# 
