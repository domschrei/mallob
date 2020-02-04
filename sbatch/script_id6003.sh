#!/bin/bash
#SBATCH --nodes=20
#SBATCH --ntasks=100
#SBATCH --cpus-per-task=4
#SBATCH --output=mallob_id6003_20x5x4_21620s_log
#SBATCH --error=mallob_id6003_20x5x4_21620s_err
#SBATCH --job-name=mallob_id6003_20x5x4_21620s
#SBATCH --partition=normal
#SBATCH --time=6:00:20
mkdir -p mallob_logs/mallob_id6003_20x5x4_21620s
echo logging into mallob_logs/mallob_id6003_20x5x4_21620s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob scenarios/scenario_all_c1 -c=1 -l=0.950 -t=4 -T=21600 -lbc=8 -time-per-instance=0 -cpuh-per-instance=56.400 -derandomize -ba=4 -g=5 -md=0 -p=5 -s=1 -v=5 -warmup -log=mallob_logs/mallob_id6003_20x5x4_21620s
echo finished
