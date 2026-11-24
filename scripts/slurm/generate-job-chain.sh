#!/bin/bash

source scripts/slurm/account.sh # $projectname , $username

jobname="$1"
sbatch_base="$2"
minjobidx="$3"
maxjobidx="$4"
numchains="$5"

matches=(/hppfs/work/$projectname/$username/logs/${jobname}-*)
if [ -d "${matches[0]}" ]; then
    echo "Matching target directory / directories already exist: $matches"
    echo "Remove or rename, then retry."
    exit 1
fi

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

for i in $(seq $minjobidx $maxjobidx); do echo $i ; done | tac > $dir/.remaining_ids

cmd="for i in {1..$numchains}; do sbatch $out_templated; done"

echo "Execute the following command:"
echo "$cmd"

