#!/bin/bash

source $ACCOUNTINFO # $projectname , $username

echo ""
echo "$username"
echo "$projectname"

sbatch_base="$1"   #"scripts/slurm/sbatch.sh"
minjobidx="$2"
maxjobidx="$3"
numchains="$4"
jobname="$5"

#cd $HOME/mallob
echo ""
echo "$jobname: $sbatch_base"
echo "$jobname: min         $minjobidx"
echo "$jobname: max         $maxjobidx"
echo "$jobname: chains      $numchains"
echo "$jobname: nodes       $DS_NODES"
echo "$jobname: $DS_BENCHMARKFILE"
echo " "

#As security check show the current mallob flags
scripts/slurm/showflags.sh "$sbatch_base"
echo " "

dir="sbatch/generated/$jobname"
mkdir -p "$dir"

out_templated="$dir/sbatch.sh"
runtime_slurmstr=$(date -d@${DS_RUNTIME} -u +%H:%M:%S)

echo "templated origin: $sbatch_base"
echo "templated target: $out_templated"
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
# escaped_benchmarkfile=$(echo "$benchmarkfile" | sed 's/\//\\\//g')
sed -i 's/$DS_BENCHMARKFILE/'$DS_BENCHMARKFILE'/g' "$out_templated"

cmd="for i in {1..$numchains}; do sbatch $out_templated; done"

echo "Execute the following command:"
echo ""
echo "$cmd"
echo ""
