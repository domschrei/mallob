
# Running Mallob on Commodity Clusters

This documentation explains how to run Mallob on commodity clusters / supercomputers with the example of SuperMUC-NG.

## Preparations

Some clusters like SuperMUC-NG do not allow internet access on their login nodes. To fetch and build Mallob and its dependencies, we need to use SSHFS (= SSH File System). At your local login point, execute these commands:

    mkdir cluster
    sshfs USERNAME@skx.supermuc.lrz.de:. cluster

Replace `USERNAME` accordingly. In the directory `cluster`, you now have a mirror of your cluster home directory at your login point. Changes in this directory will be reflected on the cluster.

Clone the Mallob repository into a subdirectory of `cluster`. Fetch Mallob's dependencies which require internet access:

    cd lib
    bash fetch_solvers.sh kclyg  # not fetch and build - only fetch!

Also fetch and/or copy any additional files you require on the cluster, such as benchmark instances.

If you are done with the `cluster` directory, do not forget to unmount:

    fusermount -u cluster

## Building Mallob

Login to the cluster. First load the modules necessary for building, like this:

    module load slurm_setup
    module unload devEnv/Intel/2019 intel-mpi
    module load gcc/9 intel-mpi/2019-gcc cmake/3.21.4 gdb

In the `mallob` directory with all dependencies fetched, you can build Mallob like this:

    ( cd lib && bash fetch_and_build_solvers.sh kclyg )
    mkdir -p build
    cd build
    CC=$(which mpicc) CXX=$(which mpicxx) cmake -DMALLOB_USE_JEMALLOC=0 ${OPTIONS} ..
    VERBOSE=1 make -j 8
    cd ..

Set `${OPTIONS}` to the build options you desire.

You should now have functional Mallob binaries on the cluster's login node.

## Submitting a Job

Job submission on most clusters, including SuperMUC-NG, involves the SLURM scheduling system. To request the scheduling of a job, you need to submit an `sbatch` file which contains all relevant metadata of your job as well as its actual execution. 

Here is an example for an sbatch file `myjob.sh` which describes a job spanning about half an hour on 17 compute nodes (= 816 cores).
Remember to adjust the `#SBATCH` directives according to your job and to edit the places marked with `TODO`.

```bash
#!/bin/bash
#SBATCH --nodes=17
#SBATCH --ntasks-per-node=12
#SBATCH --cpus-per-task=8
#SBATCH --ntasks-per-core=2 # enables hyperthreading
#SBATCH -t 00:35:00 # 35 minutes
#SBATCH -p general # general or micro 
#SBATCH --account=YOURACCOUNT # TODO fill in your account ID
#SBATCH -J YOURJOBNAME # TODO fill in your name for the job
#SBATCH --ear=off # disable Energy-Aware Runtime for accurate performance
# SBATCH --ear-mpi-dist=openmpi # If using OpenMPI and EAR is not disabled

# SuperMUC has TWO processors with 24 physical cores each, 
# totalling 48 physical cores (96 hwthreads). See: 
# https://doku.lrz.de/download/attachments/43321076/SuperMUC-NG_computenode.png

# Load the same modules to compile Mallob
module load slurm_setup
module unload devEnv/Intel/2019 intel-mpi
module load gcc/9 intel-mpi/2019-gcc cmake/3.21.4 gdb

# Some output for debugging
module list
which mpirun
echo "#ranks: $SLURM_NTASKS"

# Use the HOME directory for logging. This is a comparably slow file system.
logdir=logs/job_$SLURM_JOB_ID
# If available, use the WORK directory for logging.
# TODO Replace YOURPROJECTNAME.
if [ -d $WORK_YOURPROJECTNAME ]; then logdir="$WORK_YOURPROJECTNAME/$logdir"; fi

# Configure the run time options of Mallob. This is just an example.
# TODO Add your own options, set -T according to the time limit given above.
taskspernode=12 # must be same as --ntasks-per-node above
cmd="build/mallob -q -T=2000 -t=4 -log=$logdir -v=4 -rpa=1 -pph=$taskspernode"

# Create the logging directory and its subdirectories. This way it is much faster
# than if all ranks attempt to do it at the same time when launching Mallob.
mkdir -p "$logdir"
oldpath=$(pwd)
cd "$logdir"
for rank in $(seq 0 $(($SLURM_NTASKS-1))); do
        mkdir $rank
done
cd "$oldpath"

# Environment variables for the job
export PATH="build/:$PATH"
export RDMAV_FORK_SAFE=1

echo JOB_LAUNCHING
echo "$cmd"
srun -n $SLURM_NTASKS $cmd
echo JOB_FINISHED
```

You can then submit the job with the command:

    sbatch myjob.sh

You can cancel a job before or during its execution with the `scancel` command.

## Watching a Job

After you submitted a job, you can monitor its status e.g. with these command:

    squeue |grep YOURACCOUNT ; squeue --start |grep YOURACCOUNT

When a job is finished, in your directory there should be a file `slurm-*.out` which contains the debugging output from the sbatch script. The logs can be found at the location provided in the sbatch script.

## Further Information

Please consult the documentation of the particular cluster you are using. For the case of SuperMUC-NG, see [this wiki](https://doku.lrz.de/display/PUBLIC/SuperMUC-NG).
