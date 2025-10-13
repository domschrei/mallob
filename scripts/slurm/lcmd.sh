#!/bin/bash
#Sanity check: Show the current mallob runtime flags
echo $(grep "^benchmarkfile" "sbatch.sh") 
flags=$(grep -A 12 -- "-mono" "sbatch.sh")
echo "$flags"

