#!/bin/bash

FLAGS=(
 -minprocs 2
 -max-lits-per-thread=35000000 
 -pre-cleanup=1
 -seed=110519 
 -s2f=solution.txt 
 -terminate-abruptly=1
 -v=3
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
INST="$HOME/PhD/instances/sat-and-minisat1m/00847fca81490df01b9e239fd6027378-bench_1614.smt2.cnf.xz" #28sec
INST="$HOME/PhD/instances/sat-and-minisat1m/006be0fb3ae0a75aac0e386c2e6c4669-bench_501.smt2.cnf.xz"

INST="$HOME/PhD/instances/sat-and-minisat1m/00be590675417eba2bb2585790ac392d-iso-brn008.shuffled-as.sat05-2933.cnf.xz" #good quick mix
# INST="$HOME/PhD/instances/sat-and-minisat1m/02da8e9305ee7b4ad33c348f46afebea-x9-03055.sat.sanitized.cnf.xz" #too easy
# INST="$HOME/PhD/instances/sat-and-minisat1m/01653db16d6cedc27f5314d680efc055-fla-komb-220-5.cnf.xz" #destructible problems!
# INST="$HOME/PhD/instances/sat-and-minisat1m/00bbdbb1bc700e4c4ceb0d6e86e33c23-glassybp-v300-s1496080651.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/02b69d0e5c2b68d5c3d650164b6c277d-Q3inK08.cnf.xz" #destructible problems
# INST="$HOME/PhD/instances/sat-and-minisat1m/0320af21bba8b8cd940a95277377803d-okgen-c1200-v300-s509783707-509783707.cnf.xz" #destructible rpoblems!
# INST="$HOME/PhD/instances/sat-and-minisat1m/003de2086be59a5fb7a7aaad0992cf47-x9-07025.sat.sanitized.cnf.xz" #nothign to find, but exits
# INST="$HOME/PhD/instances/sat-and-minisat1m/02ee4550987a8545d00d0ad14d4b215c-manthey_DimacsSorter_28_0.cnf.xz"


./scripts/run/mallob_local.sh "${FLAGS[@]}" -mono="$INST"

