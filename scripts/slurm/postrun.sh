#!/bin/bash

set -e

basedir="/hppfs/work/PROJNAME/ACCTNAME/logs" # TODO adjust according to $globallogdir_base in the sbatch file

jobname="$1"
outdir="$basedir/${jobname}"

mkdir -p "$outdir/"
mv $basedir/${jobname}-*/* "$outdir/"
mv scripts/slurm/generated/${jobname}/sbatch-*.sh "$outdir/"
echo $basedir/${jobname}-*/ | grep -oE "\-[0-9]{7}/" | grep -oE "[0-9]{7}" | while read slurmid; do
    mv slurm-${slurmid}.out "$outdir/"
done

echo "All logs and sbatch / SLURM files moved to: $outdir"
echo ""

rmdir $basedir/${jobname}-* # log directories for individual jobs
rmdir scripts/slurm/generated/${jobname}/.started.* # locks for starting jobs
rm scripts/slurm/generated/${jobname}/sbatch # base sbatch template file
rmdir scripts/slurm/generated/${jobname}
