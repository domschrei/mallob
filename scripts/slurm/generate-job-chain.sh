#!/bin/bash
#
# Example usage:
# DS_NODES=4 DS_RUNTIME=360 DS_PARTITION=micro DS_SECONDSPERJOB=300 scripts/slurm/generate-job-chain.sh \
#   sat-profiling-newplain-4nodes scripts/slurm/run-sat-chained.sh 1 500 45
#

jobname="$1"
sbatch_base="$2"
minjobidx="$3"
maxjobidx="$4"
numchains="$5"

dir="scripts/slurm/generated/$jobname"
mkdir -p "$dir"

out_templated="$dir/sbatch"
runtime_slurmstr=$(date -d@${DS_RUNTIME} -u +%H:%M:%S)

cp "$sbatch_base" "$out_templated"

sed -i 's/$DS_JOBNAME/'$jobname'/g' "$out_templated"
sed -i 's/$DS_NODES/'$DS_NODES'/g' "$out_templated"
sed -i 's/$DS_RUNTIME/'$runtime_slurmstr'/g' "$out_templated"
sed -i 's/$DS_PARTITION/'$DS_PARTITION'/g' "$out_templated"
sed -i 's/$DS_SECONDSPERJOB/'$DS_SECONDSPERJOB'/g' "$out_templated"

numjobs=$(($maxjobidx - $minjobidx + 1))

cmd=""
jobidx=1
floatstopidx=0
for chainidx in $(seq 1 $numchains); do
    startidx=$jobidx
    floatstopidx=$(echo "$floatstopidx + ${numjobs}.0 / $numchains + 0.0001" | bc -l)
    stopidx=$( printf "%.0f" $floatstopidx )
    if [ $stopidx -gt $maxjobidx ]; then stopidx=$maxjobidx; fi
    if [ $startidx -gt $stopidx ]; then break; fi
    echo "Chain $chainidx : [$startidx, $stopidx]"
    
    for i in $(seq $startidx $stopidx); do
        out_step="${out_templated}-${i}.sh"
        cp "$out_templated" "$out_step"
        sed -i 's/$DS_FIRSTJOBIDX/'$i'/g' "$out_step"
        sed -i 's/$DS_LASTJOBIDX/'$maxjobidx'/g' "$out_step"
    done
    
    cmd="$cmd mkdir $dir/.started.${startidx} && sbatch ${out_templated}-${startidx}.sh ;"
    jobidx=$(($stopidx+1))
done

echo "Execute the following command:"
echo "$cmd"
