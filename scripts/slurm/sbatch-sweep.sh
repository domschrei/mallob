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

# SuperMUC has TWO processors with 24 physical cores each, totalling 48 physical cores (96 hwthreads)
# See: https://doku.lrz.de/download/attachments/43321076/SuperMUC-NG_computenode.png

module load slurm_setup; module unload devEnv/Intel/2019 intel-mpi; module load gcc/11 intel-mpi/2019-gcc cmake/3.14.5 gdb

username="$DS_USERNAME"
projname="$DS_PROJECTNAME"
starttime=$(date +%s)

# For debugging
hostname
module list
which mpirun
echo "#ranks: $SLURM_NTASKS"

build="build" # TODO your build directory for Mallob

# Environment variables
export PATH="$build/:$PATH"
export RDMAV_FORK_SAFE=1
export MALLOC_CONF="thp:always"

# HOME
globallogdir_base=logs/$DS_JOBNAME-$SLURM_JOB_ID
# WORK
if [ -d "$WORK_$DS_PROJECTNAME" ]; then globallogdir_base="$WORK_$DS_PROJECTNAME/$globallogdir_base"; fi
# SCRATCH
#if [ -d "$SCRATCH" ]; then globallogdir_base="$SCRATCH/$globallogdir_base"; fi

# Directories for writing and for storing logs
localtmpdir_base=/tmp/$DS_JOBNAME-$SLURM_JOB_ID # fast local disk
mkdir -p $localtmpdir_base $globallogdir_base

echo "globallogdir: $globallogdir_base"

# Benchmark instances, one per line
#benchmarkfile="/hppfs/work/$projname/$username/instances/hwmcc20miters/cnf/opt/pathlist.txt" # TODO 
# benchmarkfile="/hppfs/work/$projname/$username/instances/rotmul_testcase/rotmul-4-copies.txt"
benchmarkfile="$DS_BENCHMARKFILE"

if [ ! -f $benchmarkfile ]; then
    echo "Benchmark file not found!"
    exit 1
fi

echo "Benchmarkfile: $benchmarkfile"

# Diagnose number of done / active / total jobs
ndone=$(echo sbatch/generated/$DS_JOBNAME/.done.* | wc -w)
ntotal=$(($DS_LASTJOBIDX - $DS_FIRSTJOBIDX + 1))
nactive=$(squeue -u $username|grep $username|wc -l)
# All jobs already done? -> exit
if [ $ndone -ge $ntotal ]; then exit; fi

# Failsafe: Exit if an unreasonable number of jobs of this kind have launched
echo "I" >> sbatch/generated/$DS_JOBNAME/.ticks
if [ $(cat sbatch/generated/$DS_JOBNAME/.ticks|wc -l) -gt $ntotal ]; then exit; fi

change=false # track if this task makes any progress and should hence start another task

# MAIN LOOP over benchmark instances (shuffled differently for each execution)
for i in $(seq $DS_FIRSTJOBIDX $DS_LASTJOBIDX | shuf) ; do

    # already done? -> skip
    if [ -d sbatch/generated/$DS_JOBNAME/.done.$i ] ; then continue; fi
    # exit if you may be unable to finish this job in time
    if [ $(( $(date +%s) - $starttime + $DS_SECONDSPERJOB + 30 )) -gt $DS_RUNTIME ]; then 
		echo "Exit job because probably unable to finish in time"
		break; 
	fi
    # try to get a reservation for working on this job
    if [ -d sbatch/generated/$DS_JOBNAME/.reserved.$i ] && ! [[ $(find sbatch/generated/$DS_JOBNAME/.reserved.$i -newermt "8 minutes ago") ]]; then
        # reservation is old (>8 minutes) - reset it
        rmdir sbatch/generated/$DS_JOBNAME/.reserved.$i
    fi
    # unable to get a reservation? -> skip
    if ! mkdir sbatch/generated/$DS_JOBNAME/.reserved.$i 2> /dev/null ; then continue; fi

    change=true

    # Benchmark file
    f=$(cat $benchmarkfile|sed $i'q;d')

    localtmpdir="${localtmpdir_base}/tmp/$i" # make sure that proof and disk data are written on FAST, local disk
    globallogdir="${localtmpdir_base}/$i"
    outputlogdir="${globallogdir_base}"

    #echo "logdir: $globallogdir , localtmp: $localtmpdir , output: $outputlogdir"
	
	echo " "
    echo " "
    echo "jobname:  $DS_JOBNAME"
    echo "index:    $i"
    echo "logdir:   $globallogdir"
    echo "localtmp: $localtmpdir"
    echo "output:   $outputlogdir"
    echo "instance: $f"
    echo " "


    timeout=$DS_SECONDSPERJOB
    cmd="$build/mallob \
	-mono-app=SATWITHPRE \
	-satsolver=[k_]w \
	-pb=1 -pjp=999999 -pef=1 -mono=$f \
	-jwl=$timeout -T=$(($timeout+30)) -wam=60``000 -pre-cleanup=1 \
	-q=1 -log=$globallogdir -tmp=$localtmpdir -comment-outputlogdir=$outputlogdir \
	-sro=${globallogdir}/processed-jobs.out -trace-dir=${globallogdir}/ -os=1 \
	-v=4 \
  -iff=0 -s2f=${globallogdir}/model -cm=0 \
	-rpa=1 -pph=${SLURM_NTASKS_PER_NODE} -mlpt=50``000``000 -t=$((${SLURM_CPUS_PER_TASK} / 2)) \
	-isp=0 -div-phases=1 -div-noise=0 -div-seeds=1 -div-elim=0 -div-native=0 -scsd=0 \
	-scll=60 -slbdl=60 -qcll=60 -qlbdl=60 -csm=3 -cfm=3 -cfci=30 -mscf=5 -bem=1 -aim=1 \
	-rlbd=0 -ilbd=1 -randlbd=0 -scramble-lbds=0 \
	-seed=0 \
	-spd=${globallogdir}/ -spl=4 \
	-jcup=0.05 \
	-preprocess-sweep \
	-sweep-sharing-period=50 \
	-sweep-solver-verbosity=2 \
  -preprocess-sweep-priority=1.0"


    # Pre-create network-disk output directories to avoid many concurrent filesystem manips
    mkdir -p $(for rank in $(seq 0 $(($SLURM_NTASKS-1))); do echo $outputlogdir/$i/$rank; done)

    export MALLOB_GLOBALLOGDIR=$globallogdir
    export MALLOB_LOCALTMPDIR=$localtmpdir
    export MALLOB_OUTPUTLOGDIR=$outputlogdir
    export MALLOB_BUILDDIR=$build
    export MALLOB_NUMNODES=$DS_NODES

    # Assemble MPI command options
    mpicall="mpiexec -n $SLURM_NTASKS --bind-to core --map-by numa -genvall"

    # Drop hint which file we're solving
    echo "$f -> $globallogdir"

    # Launch
    echo "$(date) JOB $i LAUNCHING"

    echo $mpicall $cmd
    $mpicall bash -c "scripts/slurm/prolog.sh ; $cmd"

    sleep 3 # avoid "nodes are still busy" issue?
    $mpicall scripts/slurm/epilog.sh

    echo "$(date) JOB $i FINISHED"

    # Commit that we're done with this job
    mkdir -p sbatch/generated/$DS_JOBNAME/.done.$i

    sleep 3 # maybe a means to avoid "nodes are still busy" srun error?

done # END OF MAIN LOOP

# No progress made? => The chain is (about to be) done, so don't submit another task
if ! $change; then exit; fi

# Submit next instance, to begin after this job is done (do nothing if it fails)
sbatch --begin=now`#+$(($DS_RUNTIME+30))` sbatch/generated/$DS_JOBNAME/sbatch.sh
