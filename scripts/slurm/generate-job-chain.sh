#!/bin/bash

source scripts/slurm/account.sh # $projectname , $username

jobname="$1"
sbatch_base="$2"
minjobidx="$3"
maxjobidx="$4"
numchains="$5"

dir="sbatch/generated/$jobname"
mkdir -p "$dir"

out_templated="$dir/sbatch.sh"
runtime_slurmstr=$(date -d@${DS_RUNTIME} -u +%H:%M:%S)

cp "$sbatch_base" "$out_templated"

sed -i 's/$DS_PROJECTNAME/'$projectname'/g' "$out_templated"
sed -i 's/$DS_USERNAME/'$username'/g' "$out_templated"
sed -i 's/$DS_JOBNAME/'$jobname'/g' "$out_templated"
sed -i 's/$DS_NODES/'$DS_NODES'/g' "$out_templated"
sed -i 's/$DS_RUNTIME_SLURMSTR/'$runtime_slurmstr'/g' "$out_templated"
sed -i 's/$DS_RUNTIME/'$DS_RUNTIME'/g' "$out_templated"
sed -i 's/$DS_PARTITION/'$DS_PARTITION'/g' "$out_templated"
sed -i 's/$DS_SECONDSPERJOB/'$DS_SECONDSPERJOB'/g' "$out_templated"
sed -i 's/$DS_FIRSTJOBIDX/'$minjobidx'/g' "$out_templated"
sed -i 's/$DS_LASTJOBIDX/'$maxjobidx'/g' "$out_templated"

cmd="for i in {1..$numchains}; do sbatch $out_templated; done"

echo "Execute the following command:"
echo "$cmd"

