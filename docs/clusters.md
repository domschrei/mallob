
# Running Mallob on HPC Clusters

This documentation explains how to run Mallob on commodity clusters / supercomputers with the example of SuperMUC-NG.

## Fetching Dependencies

Some clusters like SuperMUC-NG do not allow internet access on their login nodes. Here's two options for how you can still transfer the needed dependencies to the cluster.

### Internet via Proxy
First you need to set up a SOCKS5 proxy at your local host. Here we show an example using [proxychains](https://github.com/haad/proxychains) and the port 1537. It should work equally well with the similar package [proxychains-ng](https://github.com/rofl0r/proxychains-ng), in case you have easier access to that one. On Debian/Ubuntu, proxychains might already be installed, or is available via apt. You might also need to install openssh-server.
    
    apt install proxychains openssh-sever

Next locate the proxychains.conf file and add ``socks5  127.0.0.1       1537`` as a new line. It is usually located at either ``/etc/proxychains.conf`` or ``/usr/local/proxychains.conf``.

    [ProxyList]
    # add proxy here ...
    # meanwhile
    # defaults set to "tor"
    #socks4         127.0.0.1 9050 
    socks5  127.0.0.1       1537   

To be extra sure also explicitly activate ssh

    sudo systemctl start ssh
    sudo systemctl enable ssh

Now you can activate the proxy. Your local username and computername are the ones also shown in your terminal.
    
    ssh -D 1537 -N -f <local_username>@<local_computername>

To test if the proxy exists and works try any of these commands, they should all return something meaningful. The last command should ideally also point out the exact location of the detected proxychains.conf file. 

    netstat -tulnp | grep :1537
    ps aux | grep "ssh -D"
    curl --socks5 127.0.0.1:1537 https://ipinfo.io
    proxychains curl ipinfo.io

(In case you want to deactive the proxies again, or check if activating/deactivating them changes the behaviour of the test commands, get the PIDs via ``ps aux | grep "ssh -D"`` and then run ``kill <PID1> <PID2> ...``.)


Now everything is set up locally and you can connect to the cluster

    ssh -R 1537:localhost:1537 $ACCTNAME@skx.supermuc.lrz.de

At the destination, append the following to your `~/.bashrc`:

    export http_proxy="localhost:1537"
    export https_proxy="localhost:1537"
    export HTTP_PROXY="localhost:1537"
    export HTTPS_PROXY="localhost:1537"

and the following to `~/.gitconfig`:

    [https]
        proxy = https://localhost:1537
    [http]
        proxy = http://localhost:1537

Commands like `git`, `wget`, and `curl` should now be able to download content over the proxy, which should be sufficient for setting up Mallob and its dependencies.

In case the above http(s) entries dont work, an attempt can be to explicitly include the SOCKS5 standard. Most probably wget will no longer work with this more explicit naming, but curl should still work.
    
    export HTTP_PROXY="socks5://localhost:1537"
    export http_proxy="socks5://localhost:1537"
    export HTTPS_PROXY="socks5://localhost:1537"
    export https_proxy="socks5://localhost:1537"
and

    [https]
        proxy = socks5h://localhost:1537
    [http]
        proxy = socks5h://localhost:1537

### SSHFS

Alternatively, you can use SSHFS (= SSH File System). At your local login point, execute these commands:

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

    module load slurm_setup; module unload devEnv/Intel/2019 intel-mpi; module load gcc/11 intel-mpi/2019-gcc cmake/3.21.4 gdb valgrind

In the `mallob` directory with all dependencies fetched, you can build Mallob like this:

    ( cd lib && bash fetch_and_build_solvers.sh kcly )
    mkdir -p build
    cd build
    CC=$(which mpicc) CXX=$(which mpicxx) cmake -DMALLOB_USE_JEMALLOC=0 ${OPTIONS} ..
    VERBOSE=1 make -j 8
    cd ..

Set `${OPTIONS}` to the build options you desire.

You should now have functional Mallob binaries on the cluster's login node.

## Submitting a Job

Job submission on most clusters, including SuperMUC-NG, involves the SLURM scheduling system. To request the scheduling of a job, you need to submit an `sbatch` file which contains all relevant metadata of your job as well as its actual execution.

### Option A: Job Chaining

Under `scripts/slurm/`, you find some scripts allowing to chain jobs together. Essentially, you create one `sbatch` file for every single `-mono` run of Mallob (i.e., for each input instance) and then just submit a few of them initially, but each `sbatch` file features logic to submit the next un-submitted job as soon as it is done. The advantage of this approach is that every single job is very short, which allows the cluster's scheduler to squeeze them in its schedule, reducing waiting times. Also, the approach is robust w.r.t. crashes in individual runs. A downside is that job chaining requires several rounds of waiting in the job queue (as many as the chains' "depth").

The workflow is as follows:

* Adjust the files `scripts/slurm/run-sat-chained.sh` and `scripts/slurm/postrun.sh` to your liking, especially at the places marked with `TODO`.

* Generate the sbatch files as in the following example command (the last argument is the number of concurrent chains and must be smaller than the maximum number of allowed concurrent jobs per user at your cluster):

    DS_NODES=4 DS_RUNTIME=360 DS_PARTITION=micro DS_SECONDSPERJOB=300 scripts/slurm/generate-job-chain.sh \
    sat-profiling-newplain-4nodes scripts/slurm/run-sat-chained.sh 1 500 45

* Execute the command output by the previous command, then wait until all jobs have been executed.

* Execute `scripts/slurm/postrun.sh` to merge together all log directories into a single lob directory.

* Postprocess the logs in the output destination directory to your liking.

### Option B: Monolithic sbatch file

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
