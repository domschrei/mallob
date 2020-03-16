#!/bin/bash
#SBATCH --nodes=20
#SBATCH --ntasks=100
#SBATCH --cpus-per-task=4
#SBATCH --output=mallob_id7003_20x5x4_620s_log
#SBATCH --error=mallob_id7003_20x5x4_620s_err
#SBATCH --job-name=mallob_id7003_20x5x4_620s
#SBATCH --partition=normal
#SBATCH --time=0:10:20
mkdir -p mallob_logs/mallob_id7003_20x5x4_620s
echo logging into mallob_logs/mallob_id7003_20x5x4_620s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob scenarios/scenario_all_c1 -c=1 -l=0.950 -t=4 -T=600 -lbc=16 -time-per-instance=0 -cpuh-per-instance=1.567 -derandomize -ba=8 -g=1 -md=0 -p=0.100 -s=1 -v=3 -warmup -r=floor -log=mallob_logs/mallob_id7003_20x5x4_620s
echo finished
