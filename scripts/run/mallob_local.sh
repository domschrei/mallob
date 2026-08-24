#!/bin/bash

# How many sockets, how many physical cores per socket?
nb_sockets=$(lscpu | grep -E "^Socket" | awk '{print $NF}')
nb_pcores_per_socket=$(lscpu | grep -E "^Core.*per socket" | awk '{print $NF}')
nb_hwthreads_per_socket=$(( $(lscpu | grep -E "^Thread.* per core" | awk '{print $NF}') * $nb_pcores_per_socket ))
echo "c Detected $nb_sockets sockets, with $nb_pcores_per_socket cores ($nb_hwthreads_per_socket hardware threads) per socket."

# How many processes and threads are we launching?
nb_procs=$nb_sockets
nb_threads=$nb_pcores_per_socket
nb_procs_min=1
# Allow the config script to request a minimum number of processes
if [ "$1" = "-minprocs" ]; then
  nb_procs_min="$2"
  echo "Config file wants at least $nb_procs_min processes"
  shift 2
fi
while [ $nb_threads -gt 38 ] || [ $nb_procs -lt $nb_procs_min ]; do
    nb_procs=$((nb_procs * 2))
    nb_threads=$((nb_threads / 2))
done
echo "c Running $nb_procs processes with $nb_threads threads per process."

# Which MPIRUN are we working with?
mpiinfo=$(mpirun --version)
mpioptions=""
if echo $mpiinfo | grep -q "Open MPI"; then
    # Open MPI
    echo "c Detected OpenMPI."
    mpioptions="$mpioptions --bind-to core --map-by ppr:${nb_procs}:node:pe=$nb_threads -np $nb_procs"
elif echo $mpiinfo | grep -qE "^HYDRA build details:"; then
    # MPICH
    echo "c Detected MPICH."
    mpioptions="$mpioptions -ppn $nb_procs -bind-to core:$nb_threads"
elif echo $mpiinfo | grep -q "Intel(R) MPI"; then
    # Intel MPI
    echo "c Detected Intel(R) MPI."
    mpioptions="$mpioptions -ppn $nb_procs -genv I_MPI_PIN_DOMAIN=${nb_threads}:core"
else
    echo "c WARNING: Unable to classify local MPI implementation - trying default configuration."
    mpioptions="$mpioptions -np $nb_procs"
fi

if [ "x$1" == "x" ]; then
    echo "c No options provided. Usage examples:"
    echo "c $0 -mono=instances/r3unsat_300.cnf  # SAT solving (default, simple setup)"
    echo "c $0 "'$(scripts/presets/satcomp2026-quick.sh)'" -mono=instances/r3unsat_300.cnf  # SAT solving (SAT Comp. '26 winning config, with Satsuma)"
    echo "c $0 -mono=path/to/problem.smt2 -mono-app=SMT    # SMT solving"
    echo "c $0 -mono=path/to/problem.wcnf -mono-app=MAXSAT # MaxSAT solving"
    echo "c $0 -apidir=.api/   # Scheduled mode, process tasks via JSON api"
    exit 0
fi

# Environment variables for mpirun
export RDMAV_FORK_SAFE=1

cmd="mpirun $mpioptions build/mallob -t=$nb_threads $@"
echo "c Running command:"
echo "c   $cmd"
$cmd
