#!/bin/bash

MPIOPTS="--oversubscribe" # for OpenMPI, by default
if [ ! -z "$mpiimpl" ] && [ "$mpiimpl" != "openmpi" ]; then
    MPIOPTS="" # for other MPI implementations
fi

if [ -z $1 ]; then
    echo "Usage: $0 [--no-cleanup|--np <num-procs>] <cnf-input> [<mallob-options>]"
fi

# Parse arguments
cleanup=true
numprocs=2
input_cnf=""
fwd_args=""
while [ x"$1" != x ]; do
    arg="$1"
    if [ "$arg" == "--no-cleanup" ]; then
        cleanup=false
    elif [ "$arg" == "--np" ]; then
        shift 1
        numprocs="$1"
    elif [ -z "$input_cnf" ]; then
        input_cnf="$arg"
    else
        echo "Forward arg $arg"
        fwd_args="${fwd_args} $arg"
    fi
    shift 1
done

if [ -z "$input_cnf" ]; then
    echo "Provide a CNF file."
    exit 1
fi

p_mallob_proof="proof.lrat" # compressed combined proof output by Mallob

###############################################################################
# Helper functions
###############################################################################

function direct_child_pids() {
    ps -o pid= --ppid "$1"
} 
function list_descendants() {
    local children=$(direct_child_pids "$1")
    for pid in $children; do list_descendants "$pid"; done
    echo "$children"
}
function recursive_child_pids() {
    list_descendants "$1" | tr '\n' ' '|sed 's/ \+/ /g'
}
function exec_and_measure_time() {
    name="$1"
    shift 1
    start=$(date +%s.%N)
    $@
    end=$(date +%s.%N)
    LC_ALL=C printf "TIMING $name %.4f\n" $(echo $start $end|awk '{print $2-$1}') > .timing.$name
}

###############################################################################
# Execution
###############################################################################

# Start timing
starttime=$(date +%s.%N)

# Clean up previous temporary files
rm .mallob_result proof.lrat .timing.* 2>/dev/null

# Stage I: Run Mallob
logdir="certunsattest-$(date +%s)"
exec_and_measure_time "mallob" mpirun -np $numprocs $MPIOPTS build/mallob -t=2 -mono=$input_cnf -log=$logdir -v=3 -proof=$p_mallob_proof -proof-dir=$logdir -uninvert-proof=0 $fwd_args

# Extract Mallob's result from a file it wrote.
result=$(cat .mallob_result 2>/dev/null || echo "0")

if [ x"$result" == x"20" ] ; then
    # UNSAT found: Full proof pipeline in use

    # Stage II: Check proof
    exec_and_measure_time "check" build/standalone_lrat_checker $input_cnf $p_mallob_proof --reversed

    # -> Wait for all direct children to finish.
    wait $(direct_child_pids $$) 2>/dev/null
fi

# SAT or no result found?
# -> Clean up proof pipeline by killing remaining children.
kill $(recursive_child_pids $$) 2>/dev/null

# Stop timing
endtime=$(date +%s.%N)
elapsed=$(echo $starttime $endtime|awk '{printf("%.4f\n", $2-$1)}')

# Output stats
echo "MALLOB_RESULT $result"
cat .timing.*
echo "TIMING total $elapsed"

# Clean up this run's temporary files
if $cleanup; then 
    rm .mallob_result .timing.* 2>/dev/null
    rm -rf ./certunsattest-*/ 2>/dev/null
fi
