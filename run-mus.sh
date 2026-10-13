#!/bin/bash


OUT_DIR=$HOME/PhD/logsntraces/

# INST="$HOME/PhD/instances/sat-and-minisat1m/0225fa9581c622e5abbc8497a97edf4e-fla-qhid-360-4.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/01d037bf22a943430790eedd667f415e-60-128351.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/000a41cdca43be89ed62ea3abf2d0b64-snw_13_9_pre.cnf.xz"
# INST="$HOME/PhD/instances/sat-and-minisat1m/00847fca81490df01b9e239fd6027378-bench_1614.smt2.cnf.xz" #28sec SAT

# INST="$HOME/PhD/instances/tmp2024/12b4a08e412a3bffb513ca65639c7c69-Folkman-175-7416734.sanitized.cnf"
# INST="$HOME/PhD/instances/sat-and-minisat1m/006be0fb3ae0a75aac0e386c2e6c4669-bench_501.smt2.cnf.xz"
# INST="$HOME/PhD/instances/miters/hwmcc12miters/cnf/xits/opt/6s127.cnf.xz" #trickle

# INST="$HOME/PhD/instances/sat25/d88c6afc13cdad0e2c8371a879692b39-battleship-13-13-unsat.cnf.xz" #suddenly endless
# INST="$HOME/PhD/instances/sat25/0a8a4c28d27228e954354ea0a6e7f16c-sum_of_three_cubes_42_known_representation.cnf.xz"
# INST="$HOME/PhD/instances/sat25/1c30806c5aa9cd54bf823c20fa078243-oddball_26_5_ttf.normalised.cnf.xz"
# INST="$HOME/PhD/instances/sat25/9a839badecb20dcf505ec79eedd3753a-anbul-dated-5-15-u.cnf.xz"

# INST="$HOME/PhD/instances/sat-and-minisat1m/00be590675417eba2bb2585790ac392d-iso-brn008.shuffled-as.sat05-2933.cnf.xz" #good quick mix
# INST="$HOME/PhD/instances/sat23/28e45a3e488ddac73b6116cc6f3e67e0-g2-T99.2.0.cnf" #big CCC
# INST="$HOME/PhD/instances/sat23/65b1e2253626af4a72b954f8186a5880-hash_table_find_safety_size_10.cnf" #big CCC
# INST="$HOME/PhD/instances/sat23/0982db5b0b15642e79e8242881529b39-REGRandom-K4-L3-Seed15.cnf.xz" #isDestructible problem
# INST="$HOME/PhD/instances/sat-and-minisat1m/0151782431fa0ec1f855523721c52682-58-122783.cnf.xz" #lot of lagging
#
INST="$HOME/PhD/instances/MUS11/all/23.shuffled.cnf"

FLAGS=(
 -minprocs 2
 -max-lits-per-thread=35000000 
 -os=1
 -trace-dir=$OUT_DIR/traces
 -log=$OUT_DIR/logs/
 -spd=$OUT_DIR/logs/
 -tmp=$OUT_DIR/tmp/
 -mono-app=MUS
 -v=3
)

echo "${FLAGS[@]}"

#clean old logs and traces
$HOME/PhD/logsntraces/clean.sh
cp -r config/ "$OUT_DIR"

grep "CC=" ./lib/kissat/build/makefile

./scripts/run/mallob_local.sh "${FLAGS[@]}" -mono="$INST" -t=2

