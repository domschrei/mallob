#!/bin/bash
#Sanity check: Show the current mallob runtime flags
# echo $(grep "^benchmarkfile" "scripts/slurm/sbatch.sh") 
# echo ""
# echo ""
set -e
# flags=$(grep -A 18 -- "build/mallob" "scripts/slurm/sbatch.sh")
# flags=$(grep -A 25 -- "build/mallob" "$1")
# echo "$flags"
# echo ""

build_flags=$(grep -A 25 -- "if \[" "sweep/build_on_supermuc.sh")
echo "$build_flags"
echo "----------------------------------------------------------------------------"
echo ""
echo "----------------------------------------------------------------------------"
provided_sbatch_file=$1
flags=$(grep -A 28 -- "build/mallob" "$provided_sbatch_file")
echo "$flags"
echo "----------------------------------------------------------------------------"
echo ""

