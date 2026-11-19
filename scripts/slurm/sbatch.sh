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



date

# module unload devEnv/Intel/2019 intel-mpi; module load gcc/11 intel-mpi/2019-gcc cmake/3.14.5 gdb

source load_standard_modules.sh

username="$DS_USERNAME"
projname="$DS_PROJECTNAME"
starttime=$(date +%s)

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
if [[ $PROJECT == *toolbox* ]]; then
    # HoreKa
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

# Directories for writing and for storing logs
localtmpdir_base=${SCRATCH}/tmp/$DS_JOBNAME-$SLURM_JOB_ID # fast (?) disk
mkdir -p $localtmpdir_base $globallogdir_base

echo "SCRATCH is: $SCRATCH"
echo "TMPDIR is:  $TMPDIR"
echo "$(hostname) rank ${OMPI_COMM_WORLD_RANK:-$PMI_RANK}: $(grep Cpus_allowed_list /proc/self/status)"
echo "numactl -H"
numactl -H

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

while [ $(( $(date +%s) - $starttime + $DS_SECONDSPERJOB + 30 )) -lt $DS_RUNTIME ]; do

    # obtain lock
    while ! mkdir $lockfile 2> /dev/null ; do
        if [ $(( $(date +%s) - $starttime + $DS_SECONDSPERJOB + 30 )) -gt $DS_RUNTIME ]; then
            break # timeout
        fi
        sleep 0.1
    done
    # lock held

    # exit if you may be unable to finish another job in time
    if [ $(( $(date +%s) - $starttime + $DS_SECONDSPERJOB + 30 )) -gt $DS_RUNTIME ]; then
        rmdir $lockfile # release lock
        break
    fi

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

    # Need cjc=0 when using MallobSweep and MallobSAT as sequential actors,
    # otherwise there were some crashes because the _clause_comm couldnt properly finish,
    # which maybe was due to weird overlap between the finishing MallobSweep and starting MallobSAT,
    # where temporarliy the _clause_comm was subscribed to 2 jobs, but one of them then immediately left...?

    # -preprocess-config=config/satwithpre/actors_sweepfirst.json \
    # TODO Configure Mallob

    if [[ -n "$DS_COPYTOTMP" ]]; then
      t_start=$(date +%s.%N)
      echo "COPYTOTMP_START $(date +"%T.%3N")"
      buffered_dir="$SCRATCH/buffercopy/$SLURM_JOB_ID/$i"   # unique per job and instance, to avoid clashes 
      mkdir -p "$buffered_dir"
      cp --parents "$f" "$buffered_dir"  # we want to remember the full original source path, to have faithful logs
      f="${buffered_dir%/}/${f#/}"       # remove double slashes
      t_end=$(date +%s.%N)
      echo "COPYTOTMP_END   $(date +"%T.%3N")"
      awk -v s="$t_start" -v e="$t_end" 'BEGIN { printf "COPYTOTMP_DURATION %.3f s\n", e - s }'
      echo "NEW INSTANCE PATH $f"
      [[ -s "$f" ]] || echo "ERROR: buffered instance missing or empty: $f"
    fi
    
    # -preprocess-config=config/satwithpre/actors_M_SM_SKM.json \
    # -sat-config-files=config/sat/kissat-plaincheap.json,config/sat/kissat-exportmore.json \
    # -sat-config-files=config/sat/kissat-plaincheap.json \
    # -sat-config-files=config/sat/kissat-nowalk.json \
    timeout=$DS_SECONDSPERJOB
    preset="$(config/presets/supermuc/satwithpre | tr '\n' ' ')"
    cmd="$build/mallob $preset \
    -mono=$f -jwl=$timeout -T=$(($timeout+30)) \
    -pph=${SLURM_NTASKS_PER_NODE} -t=$((${SLURM_CPUS_PER_TASK} / 2)) \
    -log=$globallogdir -tmp=$localtmpdir -comment-outputlogdir=$outputlogdir \
    -sro=${globallogdir}/processed-jobs.out \
    -trace-dir=${globallogdir}/ \
    -preprocess-config=config/satwithpre/$DS_ACTORFILE \
    -sat-config-files=
    "

    # DIVISION BY 2 on the solvercount -t=... because the slurm variable counts hwthreads (since we activated hyperthreading), but we want to spawn a SAT-Solver only for every physical core, which is half as many hwthreads that each process receives

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
    if [[ $PROJECT == *toolbox* ]]; then
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
    if [[ $PROJECT == *toolbox* ]]; then
      $mpicall scripts/slurm/epilogNew.sh
    else
      mpicleanup="mpiexec -n $((SLURM_NTASKS)) drm -q --traceless --lite --aggressive"
      mpicopy="mpiexec -n $((SLURM_NTASKS)) dcp -q"
      echo "$mpicopy $globallogdir $outputlogdir"
      $mpicopy $globallogdir $outputlogdir
      mkdir "$outputlogdir/$i/.alldone"
      echo "$mpicleanup $globallogdir"
      $mpicleanup $globallogdir
    fi
    # $mpicall scripts/slurm/epilogNew.sh

    echo "$(date) JOB $i FINISHED"

    sleep 3 # maybe a means to avoid "nodes are still busy" srun error?

done # END OF MAIN LOOP

# No progress made? => The chain is (about to be) done, so don't submit another task
if ! $change; then exit; fi

# Submit next instance, to begin after this job is done (do nothing if it fails)
sbatch --begin=now`#+$(($DS_RUNTIME+30))` sbatch/generated/$DS_JOBNAME/sbatch.sh

# clean up lod dir skeletons
if [[ $PROJECT != *toolbox* ]]; then
  $mpicleanup $globallogdir
fi
