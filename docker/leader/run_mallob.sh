#!/bin/bash

num_hosts=$(cat $1|wc -l)
processes_per_host=$(cat $1|grep -oE "slots=[0-9]+"|head -1|grep -oE "[0-9]+")

command="mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root --hostfile $1 \
--use-hwthread-cpus --map-by numa:PE=4 --bind-to hwthread --report-bindings \
`# Executable & formula input` \
mallob -mono=$2 \
`# General options` \
-zero-only-logging -sleep=1000 -t=4 -appmode=fork -nolog -v=3 -interface-fs=0 -trace-dir=competition -pipe-large-solutions=0 -processes-per-host=$processes_per_host -regular-process-allocation \
`# SAT solving options` \
-max-lits-per-thread=50000000 -strict-clause-length-limit=20 -clause-filter-clear-interval=500 -max-lbd-partition-size=2 -export-chunks=20 \
`# Kicaliglu portfolio` \
`#-clause-buffer-discount=0.9 -satsolver=kkclkkclkkclkkclccgg # 8 Kissat, 6 CaDiCaL, 4 Lingeling, 2 Glucose` \
`# Ki, Kiki portfolio` \
`-clause-buffer-discount=1.0 -satsolver=k`"

echo "run_mallob.sh : $num_hosts hosts, $processes_per_host processes per host => $(($num_hosts * $processes_per_host)) MPI processes"
echo "run_mallob.sh : EXECUTE $command"

# Workaround for MPI error messages: Read -1, expected <some number>, errno = 1
OMPI_MCA_btl_vader_single_copy_mechanism=none
export OMPI_MCA_btl_vader_single_copy_mechanism=none

# Allow MPI processes to spawn (non-communicating) subprocesses
RDMAV_FORK_SAFE=1
export RDMAV_FORK_SAFE=1

# Run the actual command
OMPI_MCA_btl_vader_single_copy_mechanism=none RDMAV_FORK_SAFE=1 $command

echo "run_mallob.sh : DONE"

/competition/cleanup
