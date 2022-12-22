#!/bin/bash

if [ -z $1 ]; then
    echo "Usage: $0 [--pipe|--force-proof-to-disk|--no-cleanup|--np <num-procs>] <cnf-input> [<mallob-options>]"
fi

# Parse arguments
use_pipes=false
force_proof_to_disk=false
cleanup=true
numprocs=2
input_cnf=""
fwd_args=""
while [ x"$1" != x ]; do
    arg="$1"
    if [ "$arg" == "--pipe" ]; then
        use_pipes=true
    elif [ "$arg" == "--force-proof-to-disk" ]; then
        force_proof_to_disk=true
    elif [ "$arg" == "--no-cleanup" ]; then
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

# Declare names of pipes
p_preprocessor_output=".preprocessor-output.pipe" # preprocessed formula
p_id_map=".id-map.pipe" # ID map of the preprocessor
p_mallob_proof=".mallob-proof.pipe" # compressed combined proof output by Mallob
p_renumbered_proof=".renumbered-proof.pipe" # compressed renumbered proof
p_preprocess_proof=".preprocess-proof-dec.pipe" # uncompressed proof from preprocessor
p_compressed_preprocess_proof=".preprocess-proof.pipe" # compressed proof from preprocessor
p_preprocessed_header=".preprocessed-header.pipe" # header line of the preprocessed formula
p_final_compressed_proof=".final-compressed-proof.pipe" # compressed final proof
p_final_proof=".final-proof.pipe" # decompressed final proof

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
# Stages to perform, with input and output pipes indicated.
###############################################################################

# Inputs: -
# Outputs: p_preprocess_proof, p_id_map, p_preprocessor_output
function preprocess() {
    exec_and_measure_time "preprocess" tools/preprocess-simple/cleanup $input_cnf $p_preprocess_proof $p_id_map > $p_preprocessor_output
}

# Inputs: p_preprocessor_output
# Outputs: p_mallob_proof
function run_mallob() {
    exec_and_measure_time "mallob" mpirun -np $numprocs --oversubscribe build/mallob -t=2 -mono=$p_preprocessor_output -log=certunsattest-$(date +%s) -mempanic=0 -v=3 -dpa -ipm -cu -pof=$p_mallob_proof $fwd_args
}

# Inputs: p_preprocess_proof
# Outputs: p_compressed_preprocess_proof
function compress_preprocessing_proof() {
    exec_and_measure_time "compress_preprocess" tools/drat-trim/compress $p_preprocess_proof $p_compressed_preprocess_proof
}

# Inputs: p_id_map, p_preprocessed_header, p_mallob_proof
# Outputs: p_renumbered_proof
function renumber_proof() {
    exec_and_measure_time "renumber" tools/composition/build/renumber-proofs --binary --adjust-file $p_id_map $p_preprocessed_header $p_renumbered_proof $p_mallob_proof
}

# Inputs: p_compressed_preprocess_proof p_renumbered_proof
# Outputs: p_final_compressed_proof
function merge_complete_proof() {
    exec_and_measure_time "merge" cat $p_compressed_preprocess_proof $p_renumbered_proof > $p_final_compressed_proof
}

# Inputs: p_final_compressed_proof
# Outputs: p_final_proof
function decompress_final_proof() {
    if $force_proof_to_disk; then
        exec_and_measure_time "decompress_final" tools/drat-trim/decompress $p_final_compressed_proof > .proof.lrat
        mv .proof.lrat proof.lrat
    else
        exec_and_measure_time "decompress_final" tools/drat-trim/decompress $p_final_compressed_proof > $p_final_proof
    fi
}

# Inputs: p_final_proof
# Outputs: -
function check_proof() {
    if $force_proof_to_disk; then
        while ! [ -f proof.lrat ]; do sleep 0.05; done
        exec_and_measure_time "check" tools/drat-trim/lrat-check $input_cnf proof.lrat
    else
        exec_and_measure_time "check" tools/drat-trim/lrat-check $input_cnf $p_final_proof
    fi
}

###############################################################################
# Execution
###############################################################################

# Start timing
starttime=$(date +%s.%N)

# Clean up previous temporary files
rm .*.pipe .mallob_result proof.lrat .timing.* 2>/dev/null

if $use_pipes; then
    # Create named pipe special files.
    mkfifo $p_preprocessor_output
    mkfifo $p_id_map
    mkfifo $p_mallob_proof
    mkfifo $p_renumbered_proof
    mkfifo $p_preprocess_proof
    mkfifo $p_compressed_preprocess_proof
    mkfifo $p_preprocessed_header
    mkfifo $p_final_compressed_proof
    mkfifo $p_final_proof

    # Execute all the non-Mallob stages concurrently in separate processes.
    preprocess &
    compress_preprocessing_proof &
    renumber_proof &
    merge_complete_proof &
    decompress_final_proof &
    check_proof &

    # Run Mallob with this process.
    run_mallob
else
    # Execute all stages strictly sequentially. 
    # The ".*.pipe" files are created as actual files.
    preprocess
    run_mallob
    compress_preprocessing_proof
    renumber_proof
    merge_complete_proof
    decompress_final_proof
    check_proof
fi

# Extract Mallob's result from a file it wrote.
result=$(cat .mallob_result 2>/dev/null || echo "0")

if [ x"$result" == x"20" ] ; then
    # UNSAT found: Full proof pipeline in use
    # -> Wait for all direct children to finish.
    wait $(direct_child_pids $$) 2>/dev/null
fi

# SAT or no result found
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
    rm .*.pipe .mallob_result .timing.* 2>/dev/null
    rm -rf ./certunsattest-*/ 2>/dev/null
fi
