#!/bin/bash

FLAGS=(
 -minprocs 2
 -max-lits-per-thread=35000000 
 -pre-cleanup=1
 -seed=110519 
 -s2f=solution.txt 
 -terminate-abruptly=1
 -v=2 
 -mono-app=SATWITHPRE 
 -sat-config-dirs=config/sat/base/
 -preprocess-config=config/satwithpre/actors_sweepfirst.json
 -satsolver=k 
 -cjtcp=0
 -jcup=0.05
 -sweep-max-iterations=2
 -jc=2
)

echo "${FLAGS[@]}"


INST="$HOME/PhD/instances/sat-and-minisat1m/0225fa9581c622e5abbc8497a97edf4e-fla-qhid-360-4.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/01d037bf22a943430790eedd667f415e-60-128351.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/000a41cdca43be89ed62ea3abf2d0b64-snw_13_9_pre.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/003a77d2aa15a5f93aa2cfe79b986c9e-fclqcolor-20-15-15.shuffled-as.sat05-1270.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/00597fb425e994af7a9d224ef4d09fc4-41-118813.cnf.xz"
INST="$HOME/PhD/instances/sat-and-minisat1m/00847fca81490df01b9e239fd6027378-bench_1614.smt2.cnf.xz" #28sec
INST="$HOME/PhD/instances/sat-and-minisat1m/006be0fb3ae0a75aac0e386c2e6c4669-bench_501.smt2.cnf.xz"

./scripts/run/mallob_local.sh "${FLAGS[@]}" -mono="$INST"

