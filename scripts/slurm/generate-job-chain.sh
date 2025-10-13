#!/bin/bash

source $ACCOUNTINFO # $projectname , $username

sbatch_base="sbatch.sh"
minjobidx="$1"
maxjobidx="$2"
numchains="$3"
benchmarkfile="dummy"
jobname="$4"

cd $HOME/mallob
echo ""
echo "$jobname: sbatch_base $sbatch_base"
echo "$jobname: min         $minjobidx"
echo "$jobname: max         $maxjobidx"
echo "$jobname: chains      $numchains"
echo "$jobname: nodes       $DS_NODES"
#echo "benchmarkf  $benchmarkfile"
echo " "

#As security check also show the current mallob flags
lcmd.sh
#mallob_cmd=$(grep -A 12 -- "-mono" "$NEWSCRIPTS/sbatch.sh")
#echo "$mallob_cmd"
echo " "


dir="$HOME/mallob/sbatch/generated/$jobname"
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
escaped_benchmarkfile=$(echo "$benchmarkfile" | sed 's/\//\\\//g')
sed -i "s/\$DS_BENCHMARKFILE/$escaped_benchmarkfile/g" "$out_templated"

cmd="for i in {1..$numchains}; do sbatch $out_templated; done"

#echo "Execute the following command:"
echo "$cmd"
echo ""

~          
