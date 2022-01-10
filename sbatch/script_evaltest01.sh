#!/bin/bash
#SBATCH --nodes=103
#SBATCH --ntasks=512
#SBATCH --cpus-per-task=4
#SBATCH --output=mallob_evaltest01_103x5x4_600s_log
#SBATCH --error=mallob_evaltest01_103x5x4_600s_err
#SBATCH --job-name=mallob_evaltest01_103x5x4_600s
#SBATCH --partition=normal
#SBATCH --time=0:10:10

logdir="mallob_logs/mallob_evaltest01_103x5x4_600s"
clients="1"
options="scenarios/scenario_all_c$clients -ba=8 -c=$clients -cbbs=1500 -cbdf=0.75 -cfhl=300 -cg -cpuh-per-instance=10 -derandomize -g=1 -jc=4 -jjp -l=0.95 -ajpc=4 -log=$logdir -mcl=5 -md=0 -p=0.1 -q -r=bisec -s=1 -s2f=$logdir/solution -sleep=100 -T=600 -t=4 -time-per-instance=0 -v=3 -warmup"

mkdir -p $logdir
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'

echo JOB_LAUNCHING
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob $options
echo JOB_FINISHED
