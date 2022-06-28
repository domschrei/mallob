
# Introduction

Mallob is a platform for massively parallel and distributed on-demand processing of malleable jobs, handling their scheduling and load balancing.
Malleability means that the CPU resources allotted to a job may vary _during its execution_ depending on the system's overall load.
Mallob was tested on configurations with up to 6144 cores as described in our publications: [SAT 2021](https://dominikschreiber.de/papers/2021-sat-scalable.pdf), Euro-Par 2022 (coming soon!).

Most notably, Mallob features an engine for distributed SAT solving. 
According to the International SAT Competition [2020🥇](https://satcompetition.github.io/2020/downloads/satcomp20slides.pdf) and [2021🥇🥈🥈🥉](https://satcompetition.github.io/2021/slides/ISC2021.pdf), Mallob is currently the best approach for SAT solving on a large scale (800 physical cores) and one of the best approaches for SAT solving on a moderate scale (32 physical cores).

<hr/>

# Setup

## Prerequisites

Note that we only support Linux as an operating system.
(Some people have been successfully developing and experimenting with Mallob within the WSL.)

* CMake ≥ 3.11.4
* Open MPI (or another MPI implementation)
* GDB
* [jemalloc](https://github.com/jemalloc/jemalloc)

## Building

```
( cd lib && bash fetch_and_build_sat_solvers.sh )
mkdir -p build
cd build
CC=$(which mpicc) CXX=$(which mpicxx) cmake -DCMAKE_BUILD_TYPE=RELEASE -DMALLOB_APP_SAT=1 -DMALLOB_USE_JEMALLOC=1 -DMALLOB_LOG_VERBOSITY=4 -DMALLOB_ASSERT=1 -DMALLOB_SUBPROC_DISPATCH_PATH=\"build/\" ..
make; cd ..
```

Specify `-DCMAKE_BUILD_TYPE=RELEASE` for a release build or `-DCMAKE_BUILD_TYPE=DEBUG` for a debug build.
You can use the following Mallob-specific build options:

| Usage                                     | Description                                                                                                |
| ----------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| -DMALLOB_ASSERT=<0/1>                     | Turn on assertions (even on release builds). Setting to 0 limits assertions to debug builds.               |
| -DMALLOB_JEMALLOC_DIR=path                | If necessary, provide a path to a local installation of `jemalloc` where `libjemalloc.*` is located.       |
| -DMALLOB_LOG_VERBOSITY=<0..6>             | Only compile logging messages of the provided maximum verbosity and discard more verbose log calls.        |
| -DMALLOB_SUBPROC_DISPATCH_PATH=\\"path\\" | Subprocess executables must be located under <path> for Mallob to find. (Use `\"build/\"` by default.)     |
| -DMALLOB_USE_ASAN=<0/1>                   | Compile with Address Sanitizer for debugging purposes.                                                     |
| -DMALLOB_USE_GLUCOSE=<0/1>                | Compile with support for Glucose SAT solver (disabled by default due to licensing issues, see below).      |
| -DMALLOB_USE_JEMALLOC=<0/1>               | Compile with Scalable Memory Allocator `jemalloc` instead of default `malloc`.                             |
| -DMALLOB_APP_SAT=<0/1>                    | Compile with SAT engine. (Omit or set to zero if you have a custom application for Mallob instead.)        |

## Docker

We also provide a minimalistic Dockerfile using an Ubuntu 20.04 setup.
Run `docker build .` in the base directory after setting up Docker on your machine.
The final line of the output is the ID to run the container, e.g. by running `docker run -i -t <id> bash` and then executing Mallob interactively inside with the options of your choosing.

<hr/>

# Usage

## General

Given a single machine with two hardware threads per core, the following command executed in Mallob's base directory assigns one MPI process to each set of four physical cores (eight hardware threads) and then runs four solver threads on each MPI process.

```
RDMAV_FORK_SAFE=1 NPROCS=$(($(nproc)/8)) mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=4 build/mallob -t=4 $MALLOB_OPTIONS
```

To "daemonize" Mallob, i.e., to let it run in the background as a server for your own application(s), you can prepend `mpirun` by `nohup` and append `2>&1 > OUT &` to the whole command, creating a text file `OUT` for Mallob's output. (If you do not want this kind of output, use the "quiet" option `-q`.)
If running in the background, do not forget to `kill` Mallob (i.e., SIGTERM the `mpirun` process) after you are done.
Alternatively you can specify the number of jobs to process (with `-J=$NUM_JOBS`) and/or the time to pass (with `-T=$TIME_LIMIT_SECS`) before Mallob should terminate on its own.

For exact and clean logging, you should not rely on a textfile in which you piped Mallob's output (like `OUT` above).
Instead, specify a logging directory with `-log=<log-dir>` where separate sub-directories and files will be created for each worker / thread. Verbosity of logging can be set with the `-v` option.
All further options of Mallob can be seen by executing Mallob with the `-h` option. (This also works without the `mpirun` prefix.)

For running Mallob on distributed clusters, please also consult [the scripts and documentation from our Euro-Par 2022 software artifact](https://doi.org/10.6084/m9.figshare.20000642) as well as the user documentation of your particular cluster.

## Solve a single SAT instance

Use Mallob option `-mono=$PATH_TO_CNF` where `$PATH_TO_CNF` is the path and file name of the formula to solve (DIMACS CNF format, possibly with .xz or .lzma compression). In this mode, all processes participate in solving, overhead is minimal, and Mallob terminates immediately after the job has been processed.

## Solve multiple instances in an orchestrated manner

If you want to solve a fixed set of $n$ formulae or wish to evaluate Mallob's scheduling behavior with simulated jobs, follow these steps:

* Write the set of formulae into a text file `$INSTANCE_FILE` (one line per path).
* Configure the base properties of a job with a JSON file `$JOB_TEMPLATE`. For a plain job with default properties you can use `templates/job-template.json`.
* Configure the behavior of each job-introducing process ("client") with a JSON file `$CLIENT_TEMPLATE`. You can find the simplest possible configuration in `templates/client-template.json` and a more complex randomized configuration in `templates/client-template-random.json`. Both files contain all necessary documentation to adjust them as desired.

Then use these Mallob options:
```
-c=1 -ajpc=$MAX_PAR_JOBS -ljpc=$((2*$MAX_PAR_JOBS)) -J=$NUM_JOBS -job-desc-template=$INSTANCE_FILE -job-template=$JOB_TEMPLATE -client-template=$CLIENT_TEMPLATE -pls=0
```
where `$NUM_JOBS` is set to $n$ (if it is larger than $n$, a client cycles through the provided job descriptions indefinitely). You can set `-sjd=1` to shuffle the provided job descriptions. You can also increase the number of client processes introducing jobs by increasing the value of `-c`. However, note that the provided configuration for active jobs in the system is applied to each of the clients independently, hence the formulae provided in the instance file are not split up among the clients but rather duplicated.

## Process jobs on demand

This is the default and most general configuration of Mallob, i.e., without `-mono` or `-job-template` options.
You can manually set the number of worker processes (`-w`) and the number of client processes introducing jobs (`-c`). By default, all processes are workers (`-w=-1`) and a single process is additionally a client (`-c=1`). The $k$ client processes are always the $k$ processes of the highest ranks, and they open up file system interfaces for introducing jobs and retrieving results at the directories `.api/jobs.0/` through `.api/jobs.`$k-1$`/`.

### Introducing a Job

To introduce a job to the system, drop a JSON file in `.api/jobs.`$i$`/in/` (e.g., `.api/jobs.0/in/`) on the filesystem of the according PE structured like this:  
```
{
    "application": "SAT",
    "user": "admin", 
    "name": "test-job-1", 
    "files": ["/path/to/difficult/formula.cnf"], 
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
| user              | **yes**   | String       | A string specifying the user who is submitting the job                                                         |
| name              | **yes**   | String       | A user-unique name for this job (increment)                                                                    |
| files             | **yes***  | String array | File paths of the input to solve. For SAT, this must be a single (text file or compressed file or named pipe). |
| priority          | **yes***  | Float > 0    | Priority of the job (higher is more important)                                                                 |
| application       | **yes**   | String       | Which kind of problem is being solved; currently either of "SAT" or "DUMMY" (default: DUMMY)                   |
| wallclock-limit   | no        | String       | Job wallclock limit: combination of a number and a unit (ms/s/m/h/d)                                           |
| cpu-limit         | no        | String       | Job CPU time limit: combination of a number and a unit (ms/s/m/h/d)                                            |
| arrival           | no        | Float >= 0   | Job's arrival time (seconds) since program start; ignore job until then                                        |
| max-demand        | no        | Int >= 0     | Override the max. number of MPI processes this job should receive at any point in time (0: no limit)           |
| dependencies      | no        | String array | User-qualified job names (using "." as a separator) which must exit **before** this job is introduced          |
| interrupt         | no        | Bool         | If `true`, the job given by "user" and "name" is interrupted (for incremental jobs, just the current revision).|
| incremental       | no        | Bool         | Whether this job has multiple _increments_ / _revisions_ and should be treated as such                         |
| literals          | no        | Int array    | You can specify the set of SAT literals (for this increment) directly in the JSON.                             |
| precursor         | no        | String       | _(Only for incremental jobs)_ User-qualified job name (`<user>.<jobname>`) of this job's previous increment    |
| assumptions       | no        | Int array    | _(Only for incremental jobs)_ You can specify the set of assumptions for this increment directly in the JSON.  |
| done              | no        | Bool         | _(Only for incremental jobs)_ If `true`, the incremental job given by "precursor" is finalized and cleaned up. |

*) Not needed if `done` is set to `true`.

In the above example, a job is introduced with priority 0.7, with a wallclock limit of five minutes and a CPU limit of 10 CPUh.

For SAT solving, the input can be provided (a) as a plain file, (b) as a compressed (.lzma / .xz) file, or (c) as a named (UNIX) pipe.
In each case, you have the option of providing the payload (i) in text form (i.e., a valid CNF description), or, with field `content-mode: "raw"`, in binary form (i.e., a sequence of bytes representing integers).  
For text files, Mallob uses the common iCNF extension for incremental formulae: The file may contain a single line of the form `a <lit1> <lit2> ... 0` where `<lit1>`, `<lit2>` etc. are assumption literals.   
For binary files, Mallob reads clauses as integer sequences with separation zeroes in between.
Two zeroes in a row (i.e., an "empty clause") signal the end of clause literals, after which a number of assumption integers may be specified. Another zero signals that the description is complete.  
If providing a named pipe, make sure that (a) the named pipe is already created when submitting the job and (b) your application pipes the formula _after_ submitting the job (else it will hang indefinitely except if this is done in a separate thread).

Assumptions can also be specified directly in the JSON describing the job via the `assumptions` field (without any trailing zero). This way, an incremental application could maintain a single text file with a monotonically growing set of clauses.

The "arrival" and "dependencies" fields are useful to test a particular preset scenario of jobs: The "arrival" field ensures that the job will be scheduled only after Mallob ran for the specified amount of seconds. The "dependencies" field ensures that the job is scheduled only if all specified other jobs are already processed.

Mallob is notified by the kernel as soon as a valid file is placed in `.api/jobs.0/in/` and will immediately remove the file and schedule the job.

### Retrieving a Job Result

Upon completion of a job, Mallob writes a result JSON file under `.api/jobs.0/out/<user-name>.<job-name>.json` (you can repeatedly query the directory contents or employ a kernel-level mechanism like `inotify`).
Such a file may look like this:
```
{
    "application": "SAT",
    "cpu-limit": "10h",
    "file": "/path/to/difficult/formula.cnf",
    "name": "test-job-1",
    "priority": 0.7,
    "result": {
        "resultcode": 10,
        "resultstring": "SAT",
        "solution": [0, 1, 2, 3, 4, 5]
    },
    "stats": {
        "time": {
            "parsing": 0.03756427764892578,
            "processing": 0.07197785377502441,
            "scheduling": 0.0002980232238769531,
            "total": 0.11040472984313965
        },
        "used_cpu_seconds": 0.2633516788482666,
        "used_wallclock_seconds": 0.06638360023498535
    },
    "user": "admin",
    "wallclock-limit": "5m"
}
```
The result code is 0 is unknown, 10 if SAT, and 20 if UNSAT.
In case of SAT, the solution field contains the found satisfying assignment; in case of UNSAT, the result for an incremental job contains the set of failed assumptions.
Instead of the "solution" field, the response may also contain the fields "solution-size" and "solution-file" if the solution is large and if option `-pls` is set. In that case, your application has to read `solution-size` integers (as bytes) representing the solution from the named pipe located at `solution-file`.

<hr/>

# Programming Interfaces

Mallob can be extended in the following ways:

* New options to the application (or a subsystem thereof) can be added in `src/optionslist.hpp`.
* To add a new SAT solver to be used in a SAT solver engine, do the following:
    - Add a subclass of `PortfolioSolverInterface`. (You can use the existing implementation for any of the existing solvers and adapt it to your solver.)
    - Add your solver to the portfolio initialization in `src/app/sat/execution/engine.cpp`.
* To extend Mallob by adding another kind of application (like combinatorial search, planning, SMT, ...), please refer to the README in the `app/` directory.
* To add a unit test, create a class `test_*.cpp` in `src/test` and then add the test case to the bottom of `CMakeLists.txt`.
* To add a system test, consult the files `scripts/systest_commons.sh` and/or `scripts/systest.sh`.

<hr/>

# Licensing and remarks

Mallob can be used, changed and redistributed under the terms of the Lesser General Public License (LGPLv3), one exception being the Glucose interface which is excluded from compilation by default (see below).
**Please let us know if you require a different license for your specific use case.**

The used versions of Lingeling, YalSAT, CaDiCaL, and Kissat are MIT-licensed, as is HordeSat (the massively parallel solver system our SAT engine was based on).
The Glucose interface of Mallob, unfortunately, is non-free software due to the [non-free license of (parallel-ready) Glucose](https://github.com/mi-ki/glucose-syrup/blob/master/LICENCE). Notably, its usage in competitive events is restricted. So when compiling Mallob with `-DMALLOB_USE_GLUCOSE=1` make sure that you have read and understood these restrictions.

Within our codebase we make thankful use of the following liberally licensed projects:

* [Compile Time Regular Expressions](https://github.com/hanickadot/compile-time-regular-expressions) by Hana Dusíková, for matching particular user inputs
* [robin_hood hashing](https://github.com/martinus/robin-hood-hashing) by Martin Ankerl, for efficient unordered maps and sets
* [robin-map](https://github.com/Tessil/robin-map) by Thibaut Goetghebuer-Planchon, for efficient unordered maps and sets
* [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann, for reading and writing JSON files
* [ringbuf](https://github.com/rmind/ringbuf) by Mindaugas Rasiukevicius, for lock-free ring buffers

If you make use of Mallob in an academic setting, please cite the following two conference papers. If you can (or want to) cite only one of them, then please cite the SAT'21 paper when focusing on our SAT engine and the Euro-Par'22 paper when focusing on the scheduling aspects of our system.
```
@inproceedings{schreiber2021scalable,
  title={Scalable SAT Solving in the Cloud},
  author={Schreiber, Dominik and Sanders, Peter},
  booktitle={International Conference on Theory and Applications of Satisfiability Testing},
  pages={518--534},
  year={2021},
  organization={Springer},
  doi={10.1007/978-3-030-80223-3_35}
}
@inproceedings{sanders2022decentralized,
  title={Decentralized Online Scheduling of Malleable {NP}-hard Jobs},
  author={Sanders, Peter and Schreiber, Dominik},
  booktitle={International European Conference on Parallel and Distributed Computing},
  year={2022},
  organization={Springer},
  note={In press.}
}
```

If you want to specifically cite Mallob in the scope of an International SAT Competition, please cite: 
```
@article{schreiber2020engineering,
  title={Engineering HordeSat Towards Malleability: mallob-mono in the {SAT} 2020 Cloud Track},
  author={Schreiber, Dominik},
  journal={SAT Competition 2020},
  pages={45}
}
@article{schreiber2021mallob,
  title={Mallob in the {SAT} Competition 2021},
  author={Schreiber, Dominik},
  journal={SAT Competition 2021},
  pages={38}
}
```

Further references:

* **[Mallob IPASIR Bridge for incremental SAT solving](https://github.com/domschrei/mallob-ipasir-bridge)**
* **[Experimental data of our publications](https://github.com/domschrei/mallob-experimental-data)**
* **[Repository of our Euro-Par 2022 software artifact](https://github.com/domschrei/europar22-artifact-mallob)**
