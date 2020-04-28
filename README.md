
# mallob 

**Mal**leable **Lo**ad **B**alancer.  
**M**ultitasking **A**gi**l**e **Lo**gic **B**lackbox.  
SAT Solving for the cloud.

## Overview

mallob is a platform for the massively parallel and distributed (designed for up to around 1k nodes, 10k cores) processing of malleable jobs. 
Malleability means that the CPU resources of a job may vary _during its execution_ depending on the system's overall load. 
In its current orientation, mallob features multi-user, massively parallel, on-demand SAT solving as an application.

The system is fully decentralized and features highly randomized dynamic malleable load balancing realized over message passing (MPI). 
When a new job arrives in the system, it will randomly bounce through the system (corresponding to a random walk) until an idle process adopts it and becomes the job's initial "root" process. 
Then, with the job dynamically updating its demand, the job may receive additional nodes which form a binary tree rooted at the initial process as their central means of communication.

Each root node in the system carries basic information on their job's meta data, such as its current demand, its current volume (= #processes) or its priority. 
A balancing phase consisting of one or multiple All-Reduction operations that will be carried out either periodically or whenever the necessity arises. 
From the globally aggregated measures, each job can compute its new volume and can act accordingly by growing or shrinking its job tree.

In its current early evaluation stage, mallob features a number of _simulated client processes_ which spawn jobs, introduce them to the system, and receive a response from the system when a solution was found or a timeout was hit. 
In the future, we intend to replace simulated client processes with interfaces "from the outer world", e.g. TCP/HTTP/..., to easily connect external applications to mallob.

## Building

We use GNU Make as our build tool. Additionally, a valid MPI installation is required (e.g. OpenMPI, Intel MPI, MPICH, ...).

Go into the directory `src/hordesat` and execute `bash build.sh` which will (a) attempt to find your MPI installation and (b) call `make` with the appropriate arguments.
This generates the executable `build/mallob`.

Alternatively, you can run `mallob` in a virtualized manner using Docker. You will find a fitting `Dockerfile` in the root directory.

## Usage

You can launch mallob in one of the following two ways:

```
bash run.sh [valgrind] <num-processes> <options...>
mpirun -np <num-processes> [valgrind] <options...>
```

In both cases, the `valgrind` option is for debugging and detecting memory errors only.

mallob takes a single argument without a preceding dash, which is the name template for the _scenario file_(s). 
Each scenario file must be formatted like this:  
```
# ID Arv. Prio. Filename
1 5.00 0.3 instances/easy.cnf
2 7.58 0.2 instances/hard_unsat.cnf
3 13.37 0.99 instances/important.cnf
[...]
```
The ID and priority as well as the filename are essential for all configurations of mallob. 
Priorities must be greater than zero. 
When using the `lbc` option (see below), the arrival times are meaningless and can be set arbitrarily.

All command-line options of mallob can be seen by executing mallob without any parameters or with the `-h` option.
We now explain some of the most important options of mallob:

* `-c=<#clients>`: The amount of "external client" processes to simulate. When the provided scenario file is `path/to/file.txt`, then the program assumes the existence of separate scenario files `path/to/file.txt.0`, `path/to/file.txt.1`, ..., `path/to/file.txt.<#clients-1>`. Note that this number will be subtracted from the amount of actual worker processes within your program execution: `#processes = #workers + #clients`. 
* `-lbc=<#jobs-per-client>`: Simulates "leaky bucket clients": each client process will strive to have exactly `<#jobs-per-client>` jobs in the system at any given time. As long as the amount of active jobs of this client is lower than this number, the client will introduce new jobs as possible. In other words, the provided number is the amount of parallel _streams of jobs_ that each client wishes to be solved.
* `-v=<verbosity>`: How verbose the output should be. `-v=5` is generally the highest supported verbosity and will generate very large log files (including a report for every single P2P message). Verbosity values of 3 or 4 are more moderate. For outputting to log files only and not to stdout, use the `-q` (quiet) option.
* `-t=<#threads>`: Each mallob process will run `<#threads>` worker threads for each active job.
* `-l=<load-factor>`: A float `l ∈ (0, 1]` that determines which system load (i.e. the ratio `#busy-nodes / #nodes`) will be aimed at in the balancing computations. A load factor very close (or equal) to one may cause performance degradation due to job requests bouncing through the system without finding an empty node. A load factor close to zero will keep the majority of processes idle.
* `-T=<time-limit>`: Run the entire system for the specified amount of seconds. (Alternatively, you can always stop the program by `Ctrl+C`ing it.)
* `-cpuh-per-instance=<limit>, -time-per-instance=<limit>`: Sets the per-job resource limits before a job is timeouted. Due to some conformity issues, the CPUh `<limit>` is provided in hours whereas the wallclock time `<limit>` is provided in seconds. CPUh are measured as the theoretical _worker thread resources_ a job would have according to the balancing results assuming instant migrations.
* `-md=<max-demand>`: Limits the maximum possible demand any single job may have to `<max-demand>`.
* `-g=<growth-period>`: Make every job update its demand `d` according to `d := 2d+1` every `<growth-period>` seconds. When zero, a job commonly instantly assumes its full demand (i.e. the complete system). By default, the demand of a job is updated only when the next growth period is hit (i.e. when the demand is equal to the amount of nodes in a binary tree of depth `k`). With the option `-cg` (continuous growth), demands are updated at every integer.
* `-p=<balance-period>`: Do balancing every `<balance-period>` seconds. When `-bm=ed` is set (which is supposed to be(come) the default), this option means that balancing is done _at most_ every `<balance-period>` seconds.
* `-s=<comm-period>`: Employ job-internal communication every `<comm-period>` seconds. In the case of the Hordesat application, this currently means All-to-all clause exchanges.
* `-r=<round-mode>`: How to round the floating-point assignments calculated during the balancing phase to actual integer process counts for each job.
    * `floor`: Each assignment is rounded down. Leads to overly conservative assignments and may yield low system loads.
    * `prob`: Probabilistic rounding: An assignment `a+β` (`a` integer, `0 ≤ b < 1`) will become `a+1` with probability `β` and will become `a` with probability `1-β`. Leads to system loads close to the desired value (`-l`), but only for large runs. Introduces interruptive oscillations of job volumes.
    * `bisec`: Find some remainder `β` as the rounding cutoff point such that the resulting rounding of all assignments leads to the system utilization that is closest to the objective value (`<load-factor> * #workers`). Requires a logarithmic number of iterations to do the bisection over possible remainders.

## Evaluation

After a complete run of mallob, you can run `bash calc_runtimes.sh <path/to/logdir>` to create basic performance report files (e.g. `runtimes` and `qualified_runtimes` for the runtimes of all solved jobs, or `timeouts` for the response times of all _un_solved jobs).