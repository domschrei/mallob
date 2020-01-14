#!/bin/bash
#SBATCH --nodes=64
#SBATCH --ntasks=640
#SBATCH --cpus-per-task=2
#SBATCH --output=mallob_id3003_64x10x2_3620s_log
#SBATCH --error=mallob_id3003_64x10x2_3620s_err
#SBATCH --job-name=mallob_id3003_64x10x2_3620s
#SBATCH --partition=normal
#SBATCH --time=1:00:20
mkdir -p /home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id3003_64x10x2_3620s
echo logging into /home/fh2-project-sda/fj0219/mallob/mallob_logs/mallob_id3003_64x10x2_3620s
module load mpi/openmpi/3.1
mpirun --bind-to core --map-by core -report-bindings /home/fh2-project-sda/fj0219/mallob//build/mallob /home/fh2-project-sda/fj0219/mallob/scenarios/scenario_all_c8 -c=8 -l=0.950 -t=2 -T=3600 -lbc=8 -time-per-instance=0 -cpuh-per-instance=2 -derandomize -ba=4 -g=5 -md=0 -p=5 -s=1 -v=4 -q -warmup -log=/home/fh2-project-sda/fj0219/mallob//mallob_logs/mallob_id3003_64x10x2_3620s
echo finished
