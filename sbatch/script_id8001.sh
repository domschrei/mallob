#!/bin/bash
#SBATCH --nodes=26
#SBATCH --ntasks=260
#SBATCH --cpus-per-task=2
#SBATCH --output=mallob_id8001_26x10x2_3720s_log
#SBATCH --error=mallob_id8001_26x10x2_3720s_err
#SBATCH --job-name=mallob_id8001_26x10x2_3720s
#SBATCH --partition=normal
#SBATCH --time=1:02:00
mkdir -p mallob_logs/mallob_id8001_26x10x2_3720s
echo logging into mallob_logs/mallob_id8001_26x10x2_3720s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob scenarios/scenario_all_c4 -c=4 -l=0.950 -t=2 -T=3700 -lbc=4 -time-per-instance=0 -cpuh-per-instance=10.000 -derandomize -ba=8 -g=1 -md=0 -p=0.100 -s=1 -v=4 -warmup -jjp -cg -r=bisec -bm=ed -yield -log=mallob_logs/mallob_id8001_26x10x2_3720s
echo finished
