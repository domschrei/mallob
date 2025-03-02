#!/bin/bash
#SBATCH --nodes=$DS_NODES
#SBATCH --ntasks-per-node=2
#SBATCH --cpus-per-task=48
#SBATCH --ntasks-per-core=2 # SuperMUC-NG: enables hyperthreading
#SBATCH -t $DS_RUNTIME
#SBATCH -p $DS_PARTITION # SuperMUC-NG: general, micro, ...
#SBATCH --account=XXX # TODO enter your account here
#SBATCH -J $DS_JOBNAME
#SBATCH --ear=off # SuperMUC-NG: Needed for profiling / benchmarking
#SBATCH --switches=1 # Force a single island

# SBATCH --ear-mpi-dist=openmpi # SuperMUC-NG: For OpenMPI

# SuperMUC-NG has TWO processors with 24 physical cores each, totalling 48 physical cores (96 hwthreads)
# See: https://doku.lrz.de/download/attachments/43321076/SuperMUC-NG_computenode.png

# Provide the modules that you also use for compiling
module load slurm_setup; module unload devEnv/Intel/2019 intel-mpi; module load gcc/11 intel-mpi/2019-gcc cmake/3.14.5 gdb

# For debugging
hostname
module list
which mpirun
echo "#ranks: $SLURM_NTASKS"

# Build directory
build="build-maxsat"

# Environment variables
export PATH="$build/:$PATH"
export RDMAV_FORK_SAFE=1
export MALLOC_CONF="thp:always"

# HOME
globallogdir_base=logs/$DS_JOBNAME-$SLURM_JOB_ID
# WORK
if [ -d "$WORK_PROJNAME" ]; then globallogdir_base="$WORK_PROJNAME/$globallogdir_base"; fi # TODO adjust

# Directories for writing and for storing logs
localtmpdir_base=/tmp/$DS_JOBNAME-$SLURM_JOB_ID # fast local disk
echo "logdir: $globallogdir_base , localtmp: $localtmpdir_base , output: $outputlogdir"
mkdir -p $localtmpdir_base $globallogdir_base

# Benchmark instances, one per line
benchmarkfile="/hppfs/work/PROJNAME/ACCTNAME/instances/2023+2024.txt" # TODO enter benchmark file - one file path per line
if [ ! -f $benchmarkfile ]; then
    echo "Benchmark file not found!"
    exit 1
fi

# Usually, loop over all benchmark instances; since we chain jobs,
# this only executes one single job.
for i in $DS_FIRSTJOBIDX ; do

    # Benchmark file
    f=$(cat $benchmarkfile|sed $i'q;d')

    localtmpdir="${localtmpdir_base}/tmp/$i"
    globallogdir="${localtmpdir_base}/$i"
    outputlogdir="${globallogdir_base}"

    # TODO configure to your liking
    cbbs=$(echo "1500*${SLURM_CPUS_PER_TASK}/4"|bc -l)
    cbbs=${cbbs%.*}
    timeout=$DS_SECONDSPERJOB
    cmd="$build/mallob -mono=$f -jwl=$timeout -T=$(($timeout+30)) -wam=60``000 -pre-cleanup=1 \
    `#outputs` -q -log=$globallogdir -tmp=$localtmpdir -comment-outputlogdir=$outputlogdir -sro=${globallogdir}/processed-jobs.out -trace-dir=${globallogdir}/ -os=1 -v=4 -iff=0 -s2f=${globallogdir}/maxsat-model \
    `#deployment` -rpa=1 -pph=${SLURM_NTASKS_PER_NODE} -mlpt=50``000``000 -t=$((${SLURM_CPUS_PER_TASK} / 2)) \
    `#diversification` -satsolver=k_ -isp=0 -div-phases=1 -div-noise=0 -div-seeds=1 -div-elim=0 -div-native=1 -scsd=0 \
    `#sharingsetup` -scll=60 -slbdl=60 -qcll=60 -qlbdl=60 -csm=3 -cfm=3 -cfci=30 -mscf=5 -bem=1 -aim=1 -rlbd=0 -ilbd=1 -randlbd=0 -scramble-lbds=0 \
    `#sharingvolume` -s=0.5 -cbbs=$cbbs -cblm=1 -cblp=250``000 -cusv=1 \
    `#randomseed` -seed=0 \
    `#profiling` -spd=${globallogdir}/ -spl=3 \
    "

    # Pre-create network-disk output directories to avoid many concurrent filesystem manips
    mkdir -p $(for rank in $(seq 0 $(($SLURM_NTASKS-1))); do echo $outputlogdir/$i/$rank; done)

    export MALLOB_GLOBALLOGDIR=$globallogdir
    export MALLOB_LOCALTMPDIR=$localtmpdir
    export MALLOB_OUTPUTLOGDIR=$outputlogdir
    export MALLOB_BUILDDIR=$build
    export MALLOB_NUMNODES=$DS_NODES

    # Assemble MPI command options
    mpicall="mpiexec -n $SLURM_NTASKS --bind-to core --map-by numa -genvall"

    # Drop which file we're solving and where we're logging to
    echo "$f -> $globallogdir"

    # Launch
    echo "$(date) JOB $i LAUNCHING"

    echo $mpicall $cmd
    $mpicall bash -c "scripts/slurm/prolog.sh ; $cmd"
    
    sleep 5 # avoid "nodes are still busy" issue that can stall execution for 5 minutes or longer
    $mpicall scripts/slurm/epilog.sh

    echo "$(date) JOB $i FINISHED"

# END OF MAIN LOOP
done

# Find the next job to submit
numjobs=$DS_LASTJOBIDX
i=$DS_FIRSTJOBIDX
for x in $(seq 1 $(($numjobs-1))) ; do
    i=$(($i+1))
    if [ $i -gt $DS_LASTJOBIDX ]; then i=1; fi
    # job already submitted?
    if ! mkdir scripts/slurm/generated/$DS_JOBNAME/.started.$i ; then continue; fi
    # acquired lock to run this job next!
    echo "Acquired lock to submit instance $i"
    # submit next job in the chain - retry until there is space
    while ! sbatch scripts/slurm/generated/$DS_JOBNAME/sbatch-${i}.sh ; do sleep 1 ; done
    break
done
