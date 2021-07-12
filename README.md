
# Mallob 

Short links: [Experimental data of our SAT21 paper](https://github.com/domschrei/mallob-experimental-data) · [Animallob: Interactive visualization of experiments](https://dominikschreiber.de/animallob)

<hr/>

## Overview

Mallob is a platform for massively parallel and distributed on-demand processing of malleable jobs, handling their scheduling and load balancing.
Malleability means that the CPU resources allotted to a job may vary _during its execution_ depending on the system's overall load.

Most notably, Mallob features an engine for distributed SAT solving. 
According to the International SAT Competition [2020🥇](https://satcompetition.github.io/2020/downloads/satcomp20slides.pdf) and [2021🥇🥈](https://satcompetition.github.io/2021/slides/ISC2021.pdf), Mallob is currently the best approach for SAT solving on a large scale (800 physical cores) and one of the best approaches for SAT solving on a moderate scale (32 physical cores).

More information on the design decisions and techniques of Mallob can be found in [our SAT 2021 paper](https://dominikschreiber.de/papers/2021-sat-scalable.pdf) where we also evaluated Mallob on up to 2560 physical cores.

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

All command-line options of Mallob can be seen by executing Mallob with the `-h` option. This also works without the `mpirun` prefix.

<hr/>

## Programming Interfaces

Mallob can be extended in the following ways:

* New options to the application (or a subsystem thereof) can be added in `src/optionslist.hpp`.
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
