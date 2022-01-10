#!/bin/bash
#SBATCH --nodes=40
#SBATCH --ntasks=400
#SBATCH --cpus-per-task=2
#SBATCH --output=mallob_evaltest03_40x10x4_3600s_log
#SBATCH --error=mallob_evaltest03_40x10x4_3600s_err
#SBATCH --job-name=mallob_evaltest03_40x10x4_3600s
#SBATCH --partition=normal
#SBATCH --time=1:00:00

logdir="mallob_logs/mallob_evaltest03_40x10x4_3600s"
clients="2"
options="scenarios/scenario_multiprio_c$clients -appmode=fork -ba=8 -c=$clients -cbbs=1500 -cbdf=0.75 -cfhl=300 -cg -cpuh-per-instance=1.5 -derandomize -g=1 -jc=4 -jjp -l=0.95 -ajpc0=0 -ajpc1=1 -log=$logdir -mcl=5 -md=0 -p=0.1 -q -r=bisec -s=1 -s2f=$logdir/solution -sleep=100 -T=3590 -t=4 -time-per-instance=0 -v=3 -warmup"

mkdir -p $logdir
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'

echo JOB_LAUNCHING
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob $options
echo JOB_FINISHED
