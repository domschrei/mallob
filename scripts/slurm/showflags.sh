#!/bin/bash
#Sanity check: Show the current mallob runtime flags
echo $(grep "^benchmarkfile" "scripts/slurm/sbatch.sh") 
echo ""
echo ""
flags=$(grep -A 13 -- "build/mallob" "scripts/slurm/sbatch.sh")
echo "$flags"
echo ""

