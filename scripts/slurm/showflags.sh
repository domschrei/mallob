#!/bin/bash
#Sanity check: Show the current mallob runtime flags
# echo $(grep "^benchmarkfile" "scripts/slurm/sbatch.sh") 
# echo ""
echo ""
# flags=$(grep -A 18 -- "build/mallob" "scripts/slurm/sbatch.sh")
flags=$(grep -A 18 -- "build/mallob" "$1")
echo "$flags"
echo ""

