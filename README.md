
# mallob 

**Mal**leable **Lo**ad **B**alancer.  
**M**ultitasking **A**gi**l**e **Lo**gic **B**lackbox.  
SAT Solving for the cloud.

## Overview

mallob is a platform for the massively parallel and distributed processing of malleable jobs. 
Malleability means that the CPU resources allotted to a job may vary _during its execution_ depending on the system's overall load.
In its current orientation, mallob features multitasking massively parallel SAT solving "on demand" as an application.

The system is fully decentralized and features highly randomized dynamic malleable load balancing realized over message passing (MPI). 
When a new job arrives in the system, it will randomly bounce through the system (corresponding to a random walk) until an idle process adopts it and becomes the job's initial "root" process. 
Then, with the job dynamically updating its demand, the job may receive additional nodes which form a binary tree rooted at the initial process as their central means of communication.

Each root node in the system carries basic information on its job's meta data, such as its current demand, its current volume (= #processes) or its priority. 
A _balancing phase_ consists of one or multiple aggregations (all-reduce collective operations) of this meta data that will be carried out whenever the necessity arises.
From the globally aggregated measures, each job can compute its new volume and can act accordingly by growing or shrinking its job tree.

In its current evaluation stage, mallob features a number of _simulated client processes_ which read SAT formulae, introduce them to the system as individual jobs, and receive a response from the system when a solution was found or a timeout was hit.
Alternatively, mallob features a single instance mode (`mallob-sinst`) where only a single provided SAT formula is solved on the entire set of available cores.
In the future, we intend to replace simulated client processes with interfaces "from the outer world", e.g. TCP/HTTP/..., to easily connect external applications to mallob.

## Building

We use GNU Make as our build tool. Additionally, a valid MPI installation is required (e.g. OpenMPI, Intel MPI, MPICH, ...).

Go into the directory `src/hordesat` and execute `bash build.sh` which will (a) attempt to find your MPI installation, (b) fetch and build the necessary SAT solving backends, and (c) call mallob's `make` with the appropriate arguments.
This generates the executable `build/mallob`.

Alternatively, you can run `mallob` in a virtualized manner using Docker.
Adjust the `CMD` statement in the `Dockerfile` and/or the execution script `aws-run.sh` to fit your particular infrastructure. 

## Usage

Launch mallob as follows:

```
mpirun -np <num-processes> [more mpi options] build/mallob <mallob options>
```

### Normal mode

mallob takes a single argument without a preceding dash, which is the name template for the _scenario file_(s). 
Each scenario file must be formatted like this:

```
# ID Arv. Prio. Filename
1 5.00 0.3 instances/easy.cnf
2 7.58 0.2 instances/hard_unsat.cnf
3 13.37 0.99 instances/important.cnf
[...]
```
IDs must be positive integers. Priorities must be in the interval `(0,1]`; greater numbers denote a higher priority. 
Arrival times denote the point in time since program start where a given job _may_ enter the system; but depending on the program configuration (see `lbc` option below), the actual introduction of the job may be deferred to a later point in time.

### Single instance solving mode

mallob can be called without a scenario file if the option `-sinst=<filename>` is set.
In that case, mallob runs in single instance solving mode on the provided CNF formula file.
This option overrides a couple of options concerning balancing and job demands.

### More Options

All command-line options of mallob can be seen by executing mallob without any parameters or with the `-h` option.
The exact options mallob uses, including all non-overridden default values, are printed out on program start at default verbosity.
Here is some explanation for the most important ones:

* `-c=<#clients>`: The amount of "external client" processes to simulate. When the provided scenario file is `path/to/file.txt`, then the program assumes the existence of separate scenario files `path/to/file.txt.0`, `path/to/file.txt.1`, ..., `path/to/file.txt.<#clients-1>`. Note that this number will be subtracted from the amount of actual worker processes within your program execution: `#processes = #workers + #clients`. 
* `-lbc=<#jobs-per-client>`: Simulates "leaky bucket clients": each client process will strive to have exactly `<#jobs-per-client>` jobs in the system at any given time. As long as the amount of active jobs of this client is lower than this number, the client will introduce new jobs as possible. In other words, the provided number is the amount of _streams of jobs_ that each client wishes to be solved in parallel.
* `-v=<verbosity>`: How verbose the output should be. `-v=5` is generally the highest supported verbosity and will generate very large log files (including a report for every single P2P message). Verbosity values of 3 or 4 are more moderate. For outputting to log files only and not to stdout, use the `-q` (quiet) option.
* `-t=<#threads>`: Each mallob process will run `<#threads>` worker threads for each active job.
* `-l=<load-factor>`: A float `l ∈ (0, 1]` that determines which system load (i.e. the ratio `#busy-nodes / #nodes`) will be aimed at in the balancing computations. A load factor very close (or equal) to one may cause performance degradation due to job requests bouncing through the system without finding an empty node. A load factor close to zero will keep the majority of processes idle. In single instance solving mode, this number is automatically set to 1.
* `-T=<time-limit>`: Run the entire system for the specified amount of seconds.
* `-cpuh-per-instance=<limit>, -time-per-instance=<limit>`: Sets the per-job resource limits before a job is timeouted. Due to some conformity issues, the CPUh `<limit>` is provided in hours whereas the wallclock time `<limit>` is provided in seconds. CPUh are measured as the theoretical _worker thread resources_ a job would have according to the balancing results assuming instant migrations.
* `-md=<max-demand>`: Limits the maximum possible demand any single job may have to `<max-demand>`.
* `-g=<growth-period>`: Make every job update its demand `d` according to `d := 2d+1` every `<growth-period>` seconds. When zero, a job commonly instantly assumes its full demand (i.e. the complete system). By default, the demand of a job is updated only when the next growth period is hit (i.e. when the demand is equal to the amount of nodes in a binary tree of depth `k`). With the option `-cg` (continuous growth), demands are updated at every integer.
* `-p=<balance-period>`: Do balancing every `<balance-period>` seconds. When `-bm=ed` is set (which is the default), this option means that balancing is done _at most_ every `<balance-period>` seconds.
* `-s=<comm-period>`: Employ job-internal communication every `<comm-period>` seconds. In the case of the Hordesat application, this currently means All-to-all clause exchanges.
* `-r=<round-mode>`: How to round the floating-point assignments calculated during the balancing phase to actual integer process counts for each job.
    * `floor`: Each assignment is rounded down. Leads to overly conservative assignments and may yield low system loads.
    * `prob`: Probabilistic rounding: An assignment `a+β` (`a` integer, `0 ≤ b < 1`) will become `a+1` with probability `β` and will become `a` with probability `1-β`. Leads to system loads close to the desired value (`-l`), but only for large runs. Introduces interruptive oscillations of job volumes.
    * `bisec`: Find some remainder `β` as the rounding cutoff point such that the resulting rounding of all assignments leads to the system utilization that is closest to the objective value (`<load-factor> * #workers`). Requires a logarithmic number of iterations to do the bisection over possible remainders.
* `sleep=<microsecs>`: How many microseconds a worker main thread should sleep in between one of its loop cycles. Use 100µs (default) for a very agile system handling messages quickly. You can usually use higher values (1-10ms) for single instance solving mode to give the solver threads a bit more computation time.

## Evaluation

After a complete run of mallob, you can run `bash calc_runtimes.sh <path/to/logdir>` to create basic performance report files (e.g. `runtimes` and `qualified_runtimes` for the runtimes of all solved jobs, or `timeouts` for the response times of all _un_solved jobs).

## Programming Interfaces

The following "interfaces" are included and/or planned:

* To extend mallob by adding another kind of job solving engine, a subclass of `Job` (see `src/app/job.hpp`) must be created and an additional case must be added to `JobDatabase::createJob` (see `src/data/job_database.cpp`). To make the job database acknowledge what kind of job is introduced in a program run with several kinds of jobs, the `JobDescription` structure should be extended by a corresponding flag (and be on either end of the serialization so that it can be read directly). Finally, the `Client` class must be extended to read and introduce this new kind of jobs.
* To add another kind of SAT solving engine, instead of directly inheriting from `Job`, a subclass of `BaseSatJob` (see `src/app/sat/base_sat_job.hpp`) can be created that already incorporates a simple kind of clause sharing. Take a look at an implementation such as `ForkedSatJob` to see how the interface can be used.
* To add a new SAT solver to be used in a SAT solver engine, implement the interface `PortfolioSolverInterface` (see `src/app/sat/hordesat/solvers/portfolio_solver_interface.hpp`); you can use the existing implementation for `Lingeling` (`lingeling.cpp`) and adapt it to your solver.

## Remarks

This system will be published in the near future by Peter Sanders and Dominik Schreiber in a conference paper or in an academic journal article.
The solving engine used in single instance solving mode was submitted to the Cloud track of the SAT Competition 2020.

Many thanks to Armin Biere et al. for the SAT solvers Lingeling and YalSAT this system uses and to Tomáš Balyo for HordeSat, the portfolio solver this project's solver engine is built upon.

You can use mallob under the terms of the Lesser General Public License (LGPLv3). The used versions of Lingeling and YalSAT are MIT-licensed, as is HordeSat.
