#!/bin/bash
#SBATCH --nodes=103
#SBATCH --ntasks=512
#SBATCH --cpus-per-task=4
#SBATCH --output=mallob_eval01_103x5x4_28h_log
#SBATCH --error=mallob_eval01_103x5x4_28h_err
#SBATCH --job-name=mallob_eval01_103x5x4_28h
#SBATCH --partition=normal
#SBATCH --time=28:00:00
mkdir -p mallob_logs/mallob_eval01_103x5x4_28h
echo logging into mallob_logs/mallob_eval01_103x5x4_28h
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm ${MPIRUN_OPTIONS} -n ${SLURM_NTASKS} build/mallob scenarios/scenario_all_c1 -ba=8 -bm=ed -c=1 -cbbs=1500 -cbdf=0.7 -cg -cpuh-per-instance=341 -derandomize -g=1 -jc=4 -jjp -l=0.95 -lbc=4 -log=mallob_logs/mallob_eval01_103x5x4_28h -md=0 -p=0.1 -q -r=bisec -s=1 -s2f=mallob_logs/mallob_eval01_103x5x4_28h/solution -sleep -T=100700 -t=4 -time-per-instance=0 -v=4 -warmup
echo finished
