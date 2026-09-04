#!/bin/bash
OUT_DIR=$HOME/PhD/logsntraces/

# INST_PATH="$HOME/PhD/instances/some2024/39277cab188349aee0f229cb7341b5c5-crafted_n12_d6_c4_num23.cnf.xz"
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/iso/6s151.cnf.xz"  # 0.1sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s176.cnf.xz" # 6sec
# INST_PATH=$HOME/PhD/instances/miter/18faad09a2e931cdfb4c8d7b1f2ef35f-rotmul.miter.used-as.sat04-336.cnf

INST_PATH="$HOME/PhD/instances/MUS11/all/23.shuffled.cnf"
# INST_PATH="$HOME/PhD/instances/MUS11/all/4pipe_1_ooo.cnf"
if ! [ -z "$1" ]; then
  echo "MUSING Instance: $1"
  INST_PATH="$1"
fi

#clean old logs and traces
rm -rf $HOME/PhD/logsntraces/logs/*
rm -rf $HOME/PhD/logsntraces/traces/*
rm -rf $HOME/PhD/logsntraces/proof/*

timeout=120
NPROCS=2
threads=3
echo "$NPROCS procs, $threads per proc"

MALLOB_OPTIONS="-t=$threads \
  -v=1 \
  -mono-app=SAT \
  -mono=$INST_PATH \
  -satsolver=c \
  -trace-dir=$OUT_DIR/traces/ \
  -log=$OUT_DIR/logs/ \
  -spd=$OUT_DIR/logs/ \
  -jwl=$timeout \
  -T=$(($timeout+30)) \
  -os=1 \
  -cm=0 \
	-seed=1 \
  -spl=2 \
  -proof=$OUT_DIR/proof/merged.lrat \
  -proof-dir=$OUT_DIR/proof/ \
  -compact-proof
"



RDMAV_FORK_SAFE=1; 

echo $MALLOB_OPTIONS | tr ' ' '\n'

mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=${threads} build/mallob $MALLOB_OPTIONS


# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/nusmvdme1d3multi.cnf.xz" #0.1sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/beemndhm2b2.cnf.xz" # UNSAT in ~30sec
# INST_PATH="$HOME/PhD/instances/some23/02c6fe8483e4f4474b7ac9731772535d-ncc_none_7047_6_3_3_0_0_420.cnf.xz" #12sec on 88 cores
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/iso/6s151.cnf.xz"  # 0.1sec
# surprisingly regular ~2000 literals received by XTCS in SATWITHPRE
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
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s48.cnf.xz" # 6sec
# INST_PATH="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s12.cnf.xz" # 6sec

#easy SATs
# INST_PATH="$HOME/PhD/instances/sat-and-minisat1m/0225fa9581c622e5abbc8497a97edf4e-fla-qhid-360-4.cnf.xz" #<1s kissat
# INST_PATH="$HOME/PhD/instances/sat-and-minisat1m/01653db16d6cedc27f5314d680efc055-fla-komb-220-5.cnf.xz" #<1s kissat
# INST_PATH="$HOME/PhD/instances/sat-and-minisat1m/01d037bf22a943430790eedd667f415e-60-128351.cnf.xz" #30sec kissat
# INST_PATH="$HOME/PhD/instances/sat-and-minisat1m/037c423f56548082b1935e88c48ffdda-3col120_5_2.shuffled.cnf.xz" #actually has some equivalences
