#!/bin/bash

source scripts/slurm/account.sh # $projectname , $username

set -e

jobname="$1"
outdir="/hppfs/work/$projectname/$username/logs/${jobname}"

openids=$(cat sbatch/generated/${jobname}/.remaining_ids)
if ! [ -z "$openids" ]; then
    echo "ERROR: Remaining open job IDs: $openids"
    echo "If you are fine with this, execute \"> sbatch/generated/${jobname}/.remaining_ids\" and retry."
    exit 1
fi

mkdir -p "$outdir/"
for f in /hppfs/work/$projectname/$username/logs/${jobname}-*/*/.alldone ; do
    if [ -d "$outdir/$(basename $(dirname $f))" ]; then continue; fi
    mv $(dirname $f) "$outdir/"
done
mv sbatch/generated/${jobname}/sbatch.sh "$outdir/"
echo /hppfs/work/$projectname/$username/logs/${jobname}-*/ | grep -oE "\-[0-9]{7}/" | grep -oE "[0-9]{7}" | while read slurmid; do
    if ! [ -f slurm-${slurmid}.out ]; then
       echo "WARN: Slurm file slurm-${slurmid}.out not present"
       continue
    fi
    mv slurm-${slurmid}.out "$outdir/"
done

for globallogdir in $outdir/*/ ; do
    grep -m 1 "Program options" "$globallogdir/0/log.0" | grep -oP "mono=.*? " | sed 's/mono=//g' | awk '{print $1}' > "$globallogdir/instance.txt"
done

echo "All logs and sbatch / SLURM files moved to: $outdir"
echo ""

rm sbatch/generated/${jobname}/.ticks # tick list for job counting failsafe
rm sbatch/generated/${jobname}/.remaining_ids
rmdir sbatch/generated/${jobname}

echo ""
echo "If all went well, you can now delete the old log directories by executing:"
echo "    rm -rf /hppfs/work/$projectname/$username/logs/${jobname}-*"
