#!/bin/bash
OUT_DIR=$HOME/PhD/logsntraces/

# EXPORTMORE branch

FLAGS=(
 -minprocs 3
 -max-lits-per-thread=35000000 
 -pre-cleanup=1
 -seed=110519 
 -os=1
 -jc=2
 -spl=-1
 -jcup=0.05
 -terminate-abruptly=0
 -trace-dir=$OUT_DIR/traces
 -log=$OUT_DIR/logs/
 -spd=$OUT_DIR/logs/
 -tmp=$OUT_DIR/tmp/
 -mono-app=SAT
 -satsolver=k 
 -sat-config-dirs=config/sat/base/
 -sat-config-files=config/sat/kissat-exportmore.json
 -v=3
 -exportmore-statistics=1
)

# EXPORTMORE branch

echo "${FLAGS[@]}"

#clean old logs and traces
$HOME/PhD/logsntraces/clean.sh

grep "CC=" ./lib/kissat/build/makefile

INST="$HOME/PhD/instances/sat-and-minisat1m/0225fa9581c622e5abbc8497a97edf4e-fla-qhid-360-4.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/01d037bf22a943430790eedd667f415e-60-128351.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/000a41cdca43be89ed62ea3abf2d0b64-snw_13_9_pre.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/00847fca81490df01b9e239fd6027378-bench_1614.smt2.cnf.xz" #28sec
# INST="$HOME/PhD/instances/sat-and-minisat1m/006be0fb3ae0a75aac0e386c2e6c4669-bench_501.smt2.cnf.xz"

# INST="$HOME/PhD/instances/sat-and-minisat1m/00be590675417eba2bb2585790ac392d-iso-brn008.shuffled-as.sat05-2933.cnf.xz" #good quick mix
#

echo "EXPORTMORE branch"
./scripts/run/mallob_local.sh "${FLAGS[@]}" -mono="$INST" -t=3

