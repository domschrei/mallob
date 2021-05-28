
# Mallob 

Short links: [Experimental data of our SAT21 paper](https://github.com/domschrei/mallob-experimental-data) · [Animallob: Interactive visualization of experiments](https://dominikschreiber.de/animallob)

<hr/>

## Overview

Mallob is a platform for massively parallel and distributed processing of malleable jobs, handling their scheduling and load balancing.
Malleability means that the CPU resources allotted to a job may vary _during its execution_ depending on the system's overall load.

The current principal (and only) application of Mallob is massively parallel and distributed multi-tasking SAT solving "on demand":
Our system can be used to resolve many formulae from various users at once, providing a very generic JSON API to introduce SAT jobs to the system and retrieve a solution for them.
Mallob also features a single instance mode, _Mallob-mono_, where only a single provided SAT formula is solved on the entire set of available cores.
Using this configuration on 1600 hardware threads in parallel in an AWS environment, Mallob [scored the first place](https://satcompetition.github.io/2020/downloads/satcomp20slides.pdf#page=36) of the first Cloud Track of the international [SAT Competition 2020](https://satcompetition.github.io/2020/), solving the most instances among all solvers of all tracks.

### Job Scheduling and Load Balancing

The system is fully decentralized and features randomized dynamic malleable load balancing realized over message passing (MPI). 
When a new job arrives in the system, it will randomly bounce through the system (corresponding to a random walk) until an idle process adopts it and becomes the job's initial "root" process. 
Then, with the job dynamically updating its demand, the job may receive additional nodes which form a _job tree_, a binary tree rooted at the initial process, as their central means of communication.

Each root node in the system carries basic information on its job's meta data, such as its current demand, its current volume (= #processes) or its priority. 
A _balancing phase_ consists of one or multiple aggregations (all-reduce collective operations) of this meta data that will be carried out whenever the necessity arises.
From the globally aggregated measures, each job can compute its new volume and can act accordingly by growing or shrinking its job tree.

### SAT Solving Engine

The SAT solving engine of Mallob is based on [HordeSat](https://baldur.iti.kit.edu/hordesat/) (Balyo and Sanders 2015) which we re-engineered in various aspects to improve its performance and to handle malleability.
We employ portfolio solving using Lingeling-bcj, YalSAT, Glucose, and CaDiCaL (not yet fully supported) as possible SAT solving backends.
Diversification is done over random seeds, sparse random setting of phase variables, and native option-based diversification of solvers (in the case of Lingeling using diversifiers from Plingeling-ayv and -bcj).

All communication has been made completely asynchronous and now happens along the job tree.
We modified HordeSat's clause exchange and made it much more careful, sharing fewer clauses of higher importance in a duplicate-free manner, which saves lots of bandwidth and computation time.
The clause filtering mechanism has been reworked as well and now periodically forgets some probabilistic portion of registered clauses allowing for clauses to be re-shared after some time.
Several further performance improvements were introduced to Mallob, for instance the reduction of unnecessary syscalls compared to HordeSat.

<hr/>

## Installation

There are two options on how to obtain a functional instance of Mallob: (a) by direct installation on your Linux system, or (b) via Docker.

### Build on Linux

Mallob is built with CMake. 
Note that a valid MPI installation is required (e.g. OpenMPI, Intel MPI, MPICH, ...).
In addition, before building Mallob you must first execute `cd lib && bash fetch_and_build_sat_solvers.sh` which, as the name tells, fetches and builds all supported SAT solving libraries.

To build Mallob, execute the following usual steps:
```
mkdir -p build
cd build
cmake .. <cmake-options>
make
```
If you want to make use of Glucose as a SAT solver, use the cmake option `-DMALLOB_USE_RESTRICTED=1` (after having read the Licensing section below).
Use `-DMALLOB_USE_ASAN=1` to build Mallob with Address Sanitizer for debugging purposes.

### Docker

Alternatively, you can run Mallob in a Docker container.
Run `docker build .` in the base directory after setting up Docker on your machine.
The final line of the output is the ID to run the container, e.g. by running `docker run -i -t <id> bash` and then executing Mallob interactively inside with the options of your choosing.

<hr/>

## Usage

Mallob can be used in several modes of operation which are explained in the following.

### Launching Mallob

For any multinode computations, Mallob is launched with `mpirun`, `mpiexec` or some other MPI application launcher. In addition, Mallob supports spawning SAT solver engines in two ways: As a number of separate threads within the MPI process, or as its individual child process. This can be set with `-appmode=thread` or `-appmode=fork` respectively. Forked mode is the default mode and requires that (i) the executable `mallob_sat_process` (produced during the build) is located in a directory contained in the `PATH` environment variable and (ii) the environment variable `RDMAV_FORK_SAFE=1` must be set.

To sum up, a command to launch Mallob is usually structured like this: 
```
RDMAV_FORK_SAFE=1 PATH=.:$PATH mpirun <mpi-options> ./mallob <options>
```

### Solving a single SAT instance

Call Mallob with the option `-mono=<cnf-file>`.
Mallob will run in single instance solving mode on the provided CNF file: All available MPI processes (if any) are used with full power to resolve the formula in parallel. This option overrides a couple of options concerning balancing and job demands.

For instance, the `Mallob-mono` configuration for the SC 2020 Cloud Track roughly corresponds to the following parameter combination:
```
-mono=<input_cnf> -log=<logdir> -T=<timelim_secs> -appmode=thread -cbdf=0.75 -cfhl=300 -mcl=5 -sleep=1000 -t=4 -v=3 -satsolver=l
```
This runs four solver threads for each MPI process and writes all output to stdout as well as to the specified log directory, with moderate verbosity.

### Scheduling and solving many instances

Launch Mallob without any particular options regarding its mode of operation. Mallob then opens up a JSON API which can be used over the file system of any of the client nodes under `<base directory>/.api/`.

**Users**

The API distinguishes jobs by the user which introduced them. By default there is a user `admin` defined in `.api/users/admin.json`.
You can just use this user or create a new one and save it under `.api/users/<user-id>.json`. The priority must be larger than zero and no larger than one; a higher number gives more importance to the user's jobs.

**Job Directories**

If multiple clients are used, then the i-th client uses `.api/jobs.<i>/` as its API directory.
Therefore, if only a single client is employed (which is the default), the directory `.api/jobs.0/` is used.

**Introducing a Job**

To introduce a job to the system, drop a JSON file in `.api/jobs.0/new/` structured like this:  
```
{ 
    "user": "admin", 
    "name": "test-job-1", 
    "file": "/path/to/difficult/formula.cnf", 
    "priority": 0.7, 
    "wallclock-limit": "5m", 
    "cpu-limit": "10h",
    "arrival": 10.3,
    "dependencies": ["admin.prereq-job1", "admin.prereq-job2"],
    "incremental": false
}
```    

Here is a brief overview of all required and optional fields in the JSON API:

| Field name        | Required? | Value type   | Description                                                                                                    |
| ----------------- | :-------: | -----------: | -------------------------------------------------------------------------------------------------------------- |
| user              | **yes**   | String       | A user which is present at `.api/users/<user>.json`                                                            |
| name              | **yes**   | String       | A user-unique name for this job (increment)                                                                    |
| file              | **yes***  | String       | File path of the input formula to solve                                                                        |
| priority          | **yes***  | Float (0,1)  | Priority of the job (higher is more important)                                                                 |
| wallclock-limit   | no        | String       | Job wallclock limit: combination of a number and a unit (ms/s/m/h/d)                                           |
| cpu-limit         | no        | String       | Job CPU time limit: combination of a number and a unit (ms/s/m/h/d)                                            |
| arrival           | no        | Float >= 0   | Job's arrival time (seconds) since program start; ignore job until then                                        |
| max-demand        | no        | Int >= 0     | Override the max. number of MPI processes this job should receive at any point in time                         |
| dependencies      | no        | String array | User-qualified job names (using "." as a separator) which must exit **before** this job is introduced          |
| incremental       | no        | Bool         | Whether this job has multiple _increments_ / _revisions_ and should be treated as such                         |
| precursor         | no        | String       | _(Only for incremental jobs)_ User-qualified job name (`<user>.<jobname>`) of this job's previous increment    |
| incremental       | no        | Bool         | _(Only for incremental jobs)_ If `true`, the incremental job given by "precursor" is finalized and cleaned up. |

*) Not needed if `done` is set to `true`.

In the above example, a job is introduced with effective priority `<user-prio> * <job-prio> = 1.0 * 0.7 = 0.7`, with a wallclock limit of five minutes and a CPU limit of 10 CPUh.
The formula file must be a valid CNF file. For incremental jobs, Mallob uses the common iCNF extension: The file may contain a single line of the form:

`a <lit1> <lit2> ... 0`

where `<lit1>`, `<lit2>` etc. are assumption literals.

The "arrival" and "dependencies" fields are useful to test a particular preset scenario of jobs: The "arrival" field ensures that the job will be scheduled only after Mallob ran for the specified amount of seconds. The "dependencies" field ensures that the job is scheduled only if all specified other jobs are already processed.

Mallob is notified by the kernel as soon as the file is placed in `.api/jobs.0/new/` and will immediately move the job description to `.api/jobs/pending/` and schedule the job.

**Retrieving a Job Result**

Upon completion of a job, Mallob writes a result JSON file under `.api/jobs.0/done/<user-name>.<job-name>.json` (you can repeatedly query the directory contents or employ a kernel-level mechanism like `inotify`).
Such a file may look like this:
```
{
    "cpu-limit": "10h",
    "file": "/path/to/difficult/formula.cnf",
    "name": "test-job-1",
    "priority": 0.7,
    "result": {
        "responsetime": 0.02732086181640625,
        "resultcode": 20,
        "resultstring": "UNSAT",
        "revision": 0,
        "solution": []
    },
    "user": "admin",
    "wallclock-limit": "5m"
}

```
The result code is 0 is unknown, 10 if SAT, and 20 if UNSAT.
In case of SAT, the solution field contains the found satisfying assignment; in case of UNSAT, the result for an incremental job contains the set of failed assumptions.

### Options Overview

All command-line options of Mallob can be seen by executing Mallob without any parameters or with the `-h` option.
The exact options Mallob uses, including all non-overridden default values, are printed out on program start at default verbosity.
Here is an explanation for some important ones.

* `-c=<#clients>`: The number of designated client MPI processes. Note that this number will be subtracted from the amount of actual worker processes within your program execution: `#processes = #workers + #clients`. 
* `-lbc=<#jobs-per-client>`: Each client process will strive to have exactly `<#jobs-per-client>` jobs in the system at any given time. As long as the amount of active jobs of this client is lower than this number, the client will introduce new jobs as possible. In other words, the provided number is the amount of _streams of jobs_ that each client wishes to be solved in parallel.
* `-v=<verbosity>`: How verbose the output should be. `-v=6` is generally the highest supported verbosity and will generate very large log files (including a report for every single P2P message). Verbosity values of 3 or 4 are more moderate. For outputting to log files only and not to stdout, use the `-q` (quiet) option.
* `-t=<#threads>`: Each Mallob process will run `<#threads>` worker threads for each active job.
* `-satsolver=<seq>`: A sequence of SAT solvers which will cyclically employed on each job. `seq` must be a string where each character corresponds to a SAT solver: `l` for Lingeling, `c` for CaDiCaL, and `g` for Glucose (only if compiled accordingly, see Building). For instance, providing `-satsolver=llg` and `-t=4`, the employed solvers on a problem will be Lingeling-Lingeling-Glucose-Lingeling on the first node, Lingeling-Glucose-Lingeling-Lingeling on the second, and so on.
* `-l=<load-factor>`: A float `l ∈ (0, 1]` that determines which system load (i.e. the ratio `#busy-nodes / #nodes`) will be aimed at in the balancing computations. A load factor very close (or equal) to one may cause performance degradation due to job requests bouncing through the system without finding an empty node. A load factor close to zero will keep the majority of processes idle. In single instance solving mode, this number is automatically set to 1: in this case, there are as many job requests as there are processes and every job request will be successful at its very first hop.
* `-T=<time-limit>`: Run the entire system for the specified amount of seconds.
* `-job-cpu-limit=<limit>, -job-wallclock-limit=<limit>`: Sets the per-job resource limits before a job is timeouted. The CPU limit is provided in CPU seconds and the wallclock limit is provided in seconds. CPU resources are measured as the theoretical _worker thread resources_ a job would have according to the balancing results, assuming instant migrations. These values may be overridden if the job-specific limits set over the JSON API are more strict.
* `-md=<max-demand>`: Limits the maximum possible demand any single job may have to `<max-demand>`.
* `-g=<growth-period>`: Make every job update its demand `d` according to `d := 2d+1` every `<growth-period>` seconds. When zero, a job commonly instantly assumes its full demand (i.e. the complete system). By default, the demand of a job is updated only when the next growth period is hit (i.e. when the demand is equal to the amount of nodes in a binary tree of depth `k`). With the option `-cg` (continuous growth), demands are updated at every integer.
* `sleep=<microsecs>`: How many microseconds a worker main thread should sleep in between one of its loop cycles. Use 100µs (default) for a very agile system handling messages quickly. You can usually use higher values (1-10ms) for single instance solving mode to give the solver threads a bit more computation time.

<hr/>

## Programming Interfaces

Mallob can be extended in the following ways:

* To add a new SAT solver to be used in a SAT solver engine, implement the interface `PortfolioSolverInterface` (see `src/app/sat/hordesat/solvers/portfolio_solver_interface.hpp`); you can use the existing implementation for `Lingeling` (`lingeling.cpp`) and adapt it to your solver. Then add your solver to the portfolio initialization in `src/app/sat/hordesat/horde.cpp`.
* To implement a different kind of SAT solving engine, instead of directly inheriting from `Job`, a subclass of `BaseSatJob` (see `src/app/sat/base_sat_job.hpp`) can be created that already incorporates a kind of clause sharing. Take a look at an implementation such as `ForkedSatJob` to see how the interface can be used.
* To extend Mallob by adding another kind of job solving engine (like combinatorial search, planning, SMT, ...), a subclass of `Job` (see `src/app/job.hpp`) must be created and an additional case must be added to `JobDatabase::createJob` (see `src/data/job_database.cpp`). To make the job database acknowledge what kind of job is introduced in a program run with several kinds of jobs, the `JobDescription` structure should be extended by a corresponding flag (and be on either end of the serialization so that it can be read directly). Finally, the `Client` class must be extended to read and introduce this new kind of jobs.

<hr/>

## Licensing

In its default configuration, the source code of Mallob can be used, changed and redistributed under the terms of the Lesser General Public License (LGPLv3), one notable exception being the source file `src/app/sat/hordesat/solvers/glucose.cpp` (see below).
The used versions of Lingeling and YalSAT are MIT-licensed, as is HordeSat.

The Glucose interface of Mallob, unfortunately, is non-free software due to the [non-free license of (parallel-ready) Glucose](https://github.com/mi-ki/glucose-syrup/blob/master/LICENCE). Notably, its usage in competitive events is restricted. So when compiling Mallob with `MALLOB_USE_RESTRICTED=1` make sure that you have read and understood these restrictions.

<hr/>

## Remarks

Many thanks to Armin Biere et al. for the SAT solvers Lingeling and YalSAT this system uses by default and to Tomáš Balyo for HordeSat, the portfolio solver this project's solver engine is built upon.

Furthermore, in our implementation we make thankful use of the following projects:

* [Compile Time Regular Expressions](https://github.com/hanickadot/compile-time-regular-expressions) by Hana Dusíková, for matching particular user inputs
* [robin_hood hashing](https://github.com/martinus/robin-hood-hashing) by Martin Ankerl, for efficient unordered maps and sets
* [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann, for reading and writing JSON files

If you make use of Mallob in an academic setting, please cite this upcoming conference paper:

Schreiber, Dominik and Sanders, Peter (2021): **Scalable SAT Solving in the Cloud.** In: Proceedings of SAT 2021. To appear.  
Preprint available at: https://dominikschreiber.de/papers/2021-sat-scalable.pdf

If you want to specifically cite Mallob-mono in the scope of the International SAT Competition 2020, please cite: 

Schreiber, Dominik (2020): **Engineering HordeSat Towards Malleability: mallob-mono in the SAT 2020 Cloud Track.** SAT COMPETITION 2020: 45.  
URL: https://helda.helsinki.fi/bitstream/handle/10138/318754/sc2020_proceedings.pdf?sequence=1#page=45
