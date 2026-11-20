#!/bin/bash
#SBATCH --nodes=$DS_NODES
#SBATCH --ntasks-per-node=2
#SBATCH --cpus-per-task=48
#SBATCH --ntasks-per-core=2 # enables hyperthreading
#SBATCH -t $DS_RUNTIME_SLURMSTR
#SBATCH -p $DS_PARTITION # general or micro
#SBATCH --account=$DS_PROJECTNAME
#SBATCH -J $DS_JOBNAME
#SBATCH --ear=off # Needed for profiling / benchmarking
#SBATCH --switches=1 # Force a single island


# SBATCH --ear-mpi-dist=openmpi # For OpenMPI

# For Horeka2 we overwrite/add/remove some of these SBATCH values! 

# SuperMUC has TWO processors with 24 physical cores each, totalling 48 physical cores (96 hwthreads)
# See: https://doku.lrz.de/download/attachments/43321076/SuperMUC-NG_computenode.png


# module unload devEnv/Intel/2019 intel-mpi; module load gcc/11 intel-mpi/2019-gcc cmake/3.14.5 gdb

# Slurm can "count" a job as started earlier than the actual node executing this script here,
# measure that delay to understand how severe this delay can be
script_start=$(date +%s)
echo "SLURM  start: $(date -d @${SLURM_JOB_START_TIME:-0})" # converts an integer into the date string
echo "SLURM  end:   $(date -d @${SLURM_JOB_END_TIME:-0})"
echo "SCRIPT start: $(date -d @$script_start)"
echo "SLURM_TO_SCRIPT_DELAY $(( script_start - ${SLURM_JOB_START_TIME:-$script_start} )) s"

source load_standard_modules.sh

username="$DS_USERNAME"
projname="$DS_PROJECTNAME"
starttime=$(date +%s)

onHoreka() {
  [[ $PROJECT == *toolbox* ]]
}

# For debugging
hostname
module list
which mpirun
echo "#ranks: $SLURM_NTASKS"
date

build="build" # TODO your build directory for Mallob

# Environment variables
export PATH="$build/:$PATH"
export RDMAV_FORK_SAFE=1
export MALLOC_CONF="thp:always"

# HOME
globallogdir_base=logs/$DS_JOBNAME-$SLURM_JOB_ID
# WORK
# if [ -d "$WORK_$DS_PROJECTNAME" ]; then globallogdir_base="$WORK_$DS_PROJECTNAME/$globallogdir_base"; fi
if onHoreka ; then
    if [[ -z $HOREKAWORK ]]; then
        echo "Error: \$HOREKAWORK is not set" >&2
        exit 1
    fi
    globallogdir_base="$HOREKAWORK/$globallogdir_base"
else
    # SuperMUC Work
    if [ -d "$WORK_$DS_PROJECTNAME" ]; then 
      globallogdir_base="$WORK_$DS_PROJECTNAME/$globallogdir_base"; 
    fi
fi

# SCRATCH
#if [ -d "$SCRATCH" ]; then globallogdir_base="$SCRATCH/$globallogdir_base"; fi

echo "SCRATCH is: $SCRATCH"
echo "TMPDIR is:  $TMPDIR"
echo "$(hostname) rank ${OMPI_COMM_WORLD_RANK:-$PMI_RANK}: $(grep Cpus_allowed_list /proc/self/status)"

# Directories for writing and for storing logs
localtmpdir_base=${SCRATCH}/tmp/$DS_JOBNAME-$SLURM_JOB_ID # fast (?) disk
if onHoreka ; then
  # they advise to use $TMPDIR, though according to my logs above it also points to /scratch anyways...
  localtmpdir_base=${TMPDIR}/tmp/$DS_JOBNAME-$SLURM_JOB_ID #  
fi

mkdir -p $localtmpdir_base $globallogdir_base

# echo "numactl -H"
# numactl -H

# Benchmark instances, one per line
# benchmarkfile="/hppfs/work/$projname/$username/instances/2023+2024-unique.txt" # TODO benchmark file
benchmarkfile="$DS_BENCHMARKFILE"
if [ ! -f $benchmarkfile ]; then
    echo "Benchmark file not found!"
    exit 1
fi

# Failsafe: Exit if an unreasonable number of jobs of this kind have launched
echo "I" >> sbatch/generated/$DS_JOBNAME/.ticks
if [ $(cat sbatch/generated/$DS_JOBNAME/.ticks|wc -l) -gt $(($DS_LASTJOBIDX - $DS_FIRSTJOBIDX + 1)) ]; then exit; fi

change=false # track if this task makes any progress and should hence start another task
lockfile=sbatch/generated/$DS_JOBNAME/.lock
idfile=sbatch/generated/$DS_JOBNAME/.remaining_ids

# Slurms own variable is more accurate than our manual calculation, which is only used as a fallback
slurm_job_end=${SLURM_JOB_END_TIME:-$(( starttime + $DS_RUNTIME ))}
slurm_time_left() { echo $(( slurm_job_end - $(date +%s) )); }

# Buffer times for within Mallob (startup, teardown, ...) and for operations within this sbatch (prolog, epilog, ...)
MALLOB_OVERHEAD_BUFFER=20
SLURM_OVERHEAD_BUFFER=40

is_enough_time_remaining() { 
  (( $(slurm_time_left) >  $DS_SECONDSPERJOB + SLURM_OVERHEAD_BUFFER))
}

while is_enough_time_remaining; do

    # obtain lock
    got_lock=false
    while is_enough_time_remaining; do
        if mkdir $lockfile 2>/dev/null; then got_lock=true; break; fi
        sleep 0.1
    done
    if ! $got_lock; then 
      # lock is not ours -> NOT removing it
      # and we also ran out of time, so breaking out of the overall loop
      break; 
    fi   
    # lock held

    # extract next job from file of remaining instance IDs (should be constant-time)
    i=$(tail -n 1 $idfile)
    truncate -s -$((${#i} + 1)) $idfile # +1 for the newline

    rmdir $lockfile # release lock

    if [ -z "$i" ]; then
        # ID file is empty - all jobs should be done
        break
    fi

    change=true

    # Benchmark file
    f=$(cat $benchmarkfile|sed $i'q;d')

    localtmpdir="${localtmpdir_base}/tmp/$i" # make sure that proof and disk data are written on FAST, local disk
    globallogdir="${localtmpdir_base}/$i"
    outputlogdir="${globallogdir_base}"

    # We can be unlucky with the filesystem even a simple single file read can take more than a minute to complete.
    # To buffer such latencies such that Mallob doesn't see them (and they don't count towards Mallobs time),
    # copy the "cold" instance such that the fresh temporary copy is "warm" for Mallob to read
    if [[ -n "$DS_BUFFERCOPY" ]]; then
      buffered_dir="$SCRATCH/buffercopy/$SLURM_JOB_ID/$i"   # unique per job and instance to avoid clashes 
      t_start=$(date +%s.%N)
      echo "BUFFERCOPY_START $(date +"%T.%3N")"
      mkdir -p "$buffered_dir"
      cp --parents "$f" "$buffered_dir"  # we want to remember the full original source path 
      echo "BUFFERCOPY_END   $(date +"%T.%3N")"
      t_end=$(date +%s.%N)
      awk -v s="$t_start" -v e="$t_end" 'BEGIN { printf "BUFFERCOPY_DUR %.3f s\n", e - s }'
      f="${buffered_dir%/}/${f#/}"       # remove double slashes
      echo "NEW INSTANCE PATH $f"
      [[ -s "$f" ]] || echo "ERROR: buffered instance missing or empty: $f"
    fi

    # If we actually encounter a large filesystem latency, after copying the instance we might no longer have enough time for solving
    if ! is_enough_time_remaining; then
        echo "$(date) JOB $i SKIPPED after copy: only $(slurm_time_left) s left -> returning ID to queue"
        [[ -n "$DS_BUFFERCOPY" ]] && rm -rf "$buffered_dir"
        while ! mkdir $lockfile 2>/dev/null; do 
          sleep 0.1; 
        done
        echo "$i" >> $idfile
        rmdir $lockfile
        break
    fi

    # Only now we comitted to this instance

    echo "logdir: $globallogdir , localtmp: $localtmpdir , output: $outputlogdir"
	  echo " "
    echo " "
    echo "$(date)"
    echo "jobname:  $DS_JOBNAME"
    echo "index:    $i"
    echo "logdir:   $globallogdir"
    echo "localtmp: $localtmpdir"
    echo "output:   $outputlogdir"
    echo "instance: $f"
    echo "SLURM_NTASKS_PER_NODE" $SLURM_NTASKS_PER_NODE
    echo "SLURM_CPUS_PER_TASK: " $SLURM_CPUS_PER_TASK
    echo " "

    # TODO Configure Mallob
    #
    timeout=$DS_SECONDSPERJOB
    preset="$(config/presets/supermuc/satwithpre | tr '\n' ' ')"
    cmd="$build/mallob $preset \
    -mono=$f -jwl=$timeout -T=$(($timeout+$MALLOB_OVERHEAD_BUFFER)) \
    -pph=${SLURM_NTASKS_PER_NODE} -t=$((${SLURM_CPUS_PER_TASK} / 2)) \
    -log=$globallogdir -tmp=$localtmpdir -comment-outputlogdir=$outputlogdir \
    -sro=${globallogdir}/processed-jobs.out \
    -trace-dir=${globallogdir}/ \
    -preprocess-config=config/satwithpre/$DS_ACTORFILE \
    -sat-config-files=
    "

    # DIVISION BY 2 in the solvercount flag -t=... because the slurm variable counts hwthreads since we activated hyperthreading, 
    # but we want to spawn only spawn a SAT-Solver for every physical core, which is half of the number of hwthreads that each process receives

    # Pre-create network-disk output directories to avoid many concurrent filesystem manips
    mkdir -p $(for rank in $(seq 0 $(($SLURM_NTASKS-1))); do echo $outputlogdir/$i/$rank; done)

    export MALLOB_GLOBALLOGDIR=$globallogdir
    export MALLOB_LOCALTMPDIR=$localtmpdir
    export MALLOB_OUTPUTLOGDIR=$outputlogdir
    export MALLOB_BUILDDIR=$build
    export MALLOB_NUMNODES=$DS_NODES

    # For SuperMuc, Intel MPI bindings need to be provided via these exports.
    # Pinning each rank to one socket, 24 cores + their 24 hyperthreads (48 logical CPUs)
    # compact for a nice order
    export I_MPI_PIN=1
    export I_MPI_PIN_DOMAIN=socket
    export I_MPI_PIN_ORDER=compact

    # Assemble MPI command options
    if onHoreka ; then
      # HoreKa: Open MPI; PE for each process counts physical cores
      mpicall="mpirun -n $SLURM_NTASKS --bind-to core --map-by numa:PE=$((SLURM_CPUS_PER_TASK / 2)) --report-bindings \
        -x PATH -x RDMAV_FORK_SAFE -x MALLOC_CONF -x MALLOB_GLOBALLOGDIR -x MALLOB_LOCALTMPDIR \
        -x MALLOB_OUTPUTLOGDIR -x MALLOB_BUILDDIR -x MALLOB_NUMNODES"
    else
      # SuperMUC: Intel MPI. Factored out the pinning into I_MPI_* variables above
      mpicall="mpiexec -n $SLURM_NTASKS -genvall"
    fi

    # Drop hint which file we're solving
    echo "$f -> $globallogdir"

    # Launch
    echo "$(date) JOB $i LAUNCHING"

    echo $mpicall $cmd
    $mpicall bash -c "scripts/slurm/prolog.sh ; $cmd"

    echo "$(date) JOB $i FINISHED"
    sleep 3 # avoid "nodes are still busy" issue?
    echo "$(date) EPILOG"
    if onHoreka ; then
      # Horeka doesnt support mpifileutils, so fallback to our manual method
      $mpicall scripts/slurm/epilogNew.sh
    else
      mpicopy="mpiexec -n $((SLURM_NTASKS)) dcp -q"
      mpicleanup="mpiexec -n $((SLURM_NTASKS)) drm -q --traceless --lite --aggressive"
      if $mpicopy $globallogdir $outputlogdir; then
        mkdir "$outputlogdir/$i/.alldone"
        echo "$mpicleanup $globallogdir"
        $mpicleanup $globallogdir
      else
        echo "$(date) WARNING: copy of $globallogdir failed, keeping it on scratch" >&2
      fi
    fi

    # Clean up the buffered instance
    [[ -n "$DS_BUFFERCOPY" ]] && [[ -n "$buffered_dir" ]] && rm -rf "$buffered_dir"


    echo "$(date) JOB $i FINISHED"

    sleep 3 # maybe a means to avoid "nodes are still busy" srun error?

done # END OF MAIN LOOP

# No progress made? => The chain is (about to be) done, so don't submit another task
if ! $change; then exit; fi

# Submit next instance, to begin after this job is done (do nothing if it fails)
sbatch --begin=now`#+$(($DS_RUNTIME+$SLURM_OVERHEAD_BUFFER))` sbatch/generated/$DS_JOBNAME/sbatch.sh

