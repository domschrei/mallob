#!/bin/bash
#SBATCH --nodes=45
#SBATCH --ntasks=450
#SBATCH --cpus-per-task=2
#SBATCH --output=mallob_id5006_45x10x2_7220s_log
#SBATCH --error=mallob_id5006_45x10x2_7220s_err
#SBATCH --job-name=mallob_id5006_45x10x2_7220s
#SBATCH --partition=normal
#SBATCH --time=2:00:20
mkdir -p mallob_logs/mallob_id5006_45x10x2_7220s
echo logging into mallob_logs/mallob_id5006_45x10x2_7220s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob scenarios/scenario_all_c20 -c=20 -l=0.930 -t=1 -T=7200 -lbc=5 -time-per-instance=0 -cpuh-per-instance=0 -derandomize -ba=8 -g=0 -md=0 -p=5 -s=1 -v=5 -warmup -log=mallob_logs/mallob_id5006_45x10x2_7220s
echo finished
