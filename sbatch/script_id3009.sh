#!/bin/bash
#SBATCH --nodes=128
#SBATCH --ntasks=1280
#SBATCH --cpus-per-task=2
#SBATCH --output=mallob_id3009_128x10x2_3620s_log
#SBATCH --error=mallob_id3009_128x10x2_3620s_err
#SBATCH --job-name=mallob_id3009_128x10x2_3620s
#SBATCH --partition=normal
#SBATCH --time=1:00:20
mkdir -p /home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id3009_128x10x2_3620s
echo logging into /home/fh2-project-sda/fj0219/mallob/mallob_logs/mallob_id3009_128x10x2_3620s
module load mpi/impi/2019
module load compiler/gnu/7
export MPIRUN_OPTIONS='-binding domain='${SLURM_CPUS_PER_TASK}':compact -print-rank-map -envall'
mpiexec.hydra --bootstrap slurm $MPIRUN_OPTIONS -n ${SLURM_NTASKS} /home/fh2-project-sda/fj0219/mallob//build/mallob /home/fh2-project-sda/fj0219/mallob/scenarios/scenario_all_c16 -c=16 -l=0.950 -t=2 -T=3600 -lbc=8 -time-per-instance=0 -cpuh-per-instance=2 -derandomize -ba=4 -g=0 -md=0 -p=5 -s=1 -v=4 -q -warmup -log=/home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id3009_128x10x2_3620s
echo finished
