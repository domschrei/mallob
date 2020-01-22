#!/bin/bash
#SBATCH --nodes=21
#SBATCH --ntasks=420
#SBATCH --cpus-per-task=1
#SBATCH --output=mallob_id5000_21x20x1_21590s_log
#SBATCH --error=mallob_id5000_21x20x1_21590s_err
#SBATCH --job-name=mallob_id5000_21x20x1_21590s
#SBATCH --partition=normal
#SBATCH --time=5:59:50
mkdir -p /home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id5000_21x20x1_21590s
echo logging into /home/fh2-project-sda/fj0219/mallob/mallob_logs/mallob_id5000_21x20x1_21590s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm $MPIRUN_OPTIONS -n ${SLURM_NTASKS} /home/fh2-project-sda/fj0219/mallob//build/mallob /home/fh2-project-sda/fj0219/mallob/scenarios/scenario_all_c10 -c=10 -l=0.977 -t=1 -T=21570 -lbc=40 -time-per-instance=0 -cpuh-per-instance=0 -derandomize -ba=4 -g=5 -md=0 -p=5 -s=1 -v=5 -warmup -log=/home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id5000_21x20x1_21590s
echo finished
