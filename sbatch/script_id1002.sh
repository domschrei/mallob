#!/bin/bash
#SBATCH --nodes=10
#SBATCH --ntasks=40
#SBATCH --cpus-per-task=5
#SBATCH --output=mallob_id1002_10x4x5_21620s_log
#SBATCH --error=mallob_id1002_10x4x5_21620s_err
#SBATCH --job-name=mallob_id1002_10x4x5_21620s
#SBATCH --partition=normal
#SBATCH --time=6:00:20
mkdir -p /home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id1002_10x4x5_21620s
echo logging into /home/fh2-project-sda/fj0219/mallob/mallob_logs/mallob_id1002_10x4x5_21620s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm $MPIRUN_OPTIONS -n ${SLURM_NTASKS} /home/fh2-project-sda/fj0219/mallob//build/mallob /home/fh2-project-sda/fj0219/mallob/scenarios/scenario_all_c4 -c=4 -l=0.910 -t=4 -T=21600 -lbc=1 -time-per-instance=0 -cpuh-per-instance=10 -derandomize -ba=4 -g=0 -md=0 -p=5 -s=1 -v=4 -q -warmup -log=/home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id1002_10x4x5_21620s
echo finished
