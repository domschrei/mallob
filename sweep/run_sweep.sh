#!/bin/bash



OUT_DIR=$HOME/PhD/logsntraces/

# INST_PATH=$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/nusmvdme1d3multi.cnf.xz" #0.1sec
INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/beemndhm2b2.cnf.xz" # 6sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/iso/6s151.cnf.xz"  # 0.1sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/bob12s01.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/cmudme1.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/some2024/39fba35826ce8c87cd8e8de1969b2dd2-SGI_30_80_26_70_4-log.shuffled-as.sat03-208.cnf.xz" # 30sec, 17% after 2 rounds
# INST_PATH="$HOME/PhD/instances/some2024/39277cab188349aee0f229cb7341b5c5-crafted_n12_d6_c4_num23.cnf.xz"

# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s101.cnf.xz" 
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s103.cnf.xz"   #huge, & congruence extremely effective
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s107.cnf.xz"     #significant nr. of fixed before start/CEC (10%), &congr strong
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/nusmvdme1d16multi.cnf.xz" #had weird multiple works provided in quick succession
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/nusmvdme2d3multi.cnf.xz" #done almost immediately

# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s158.cnf.xz" #done almost immediately
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/bob12s04.cnf.xz" 
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s176.cnf.xz" # 6sec

NPROCS=2
threads=4
echo "NPROCS $NPROCS"
echo "threads per process $threads"

APP="SWEEP"
# APP="SATWITHPRE"

MALLOB_OPTIONS="-t=$threads \
  -mono-app=$APP \
  -satsolver=k_ \
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
	-jcup=0.05 \
  -sleep=10 \
	-v=4 \
  -spl=-1 \
  -cjc=0 \
  -fcll=2 \
  -swpxs=1 \
  -swpxr=1 \
  -app-comm-period=0.22 \
	-preprocess-sweep-n-sat=1 \
  -preprocess-sweep-priority=1.0 \
	-sweep-sharing-period=0.050 \
  -sweep-resweep-chance=1000 \
	-sweep-solver-verbosity=1 \
	-sweep-solver-quiet=1 \
  -sweep-initial-congruence=1 \
  -sweep-max-iterations=3 \
  -sweep-max-depth=4 \
  -sweep-min-exit-swept=0 \
  -sweep-term-no-progress=0 \
  -sweep-shuffle-work=0 \
  -sweep-to-completion=0 \
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
