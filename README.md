
# Mallob 

**[Experimental data of our publications](https://github.com/domschrei/mallob-experimental-data)**

<hr/>

## Overview

Mallob is a platform for massively parallel and distributed on-demand processing of malleable jobs, handling their scheduling and load balancing.
Malleability means that the CPU resources allotted to a job may vary _during its execution_ depending on the system's overall load.

Most notably, Mallob features an engine for distributed SAT solving. 
According to the International SAT Competition [2020🥇](https://satcompetition.github.io/2020/downloads/satcomp20slides.pdf) and [2021🥇🥈🥈🥉](https://satcompetition.github.io/2021/slides/ISC2021.pdf), Mallob is currently the best approach for SAT solving on a large scale (800 physical cores) and one of the best approaches for SAT solving on a moderate scale (32 physical cores).

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

As CMake options, specify `-DCMAKE_BUILD_TYPE=RELEASE` for a release build or `-DCMAKE_BUILD_TYPE=DEBUG` for a debug build.
In addition, you can use the following Mallob-specific build options:

| Usage                         | Description                                                                                                |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------- |
| -DMALLOB_ASSERT=<0/1>         | Turn on assertions (even on release builds). Setting to 0 limits assertions to debug builds.               |
| -DMALLOB_USE_ASAN=<0/1>       | Compile with Address Sanitizer for debugging purposes.                                                     |
| -DMALLOB_USE_GLUCOSE=<0/1>    | Compile with support for Glucose SAT solver (disabled by default due to licensing issues, see below).      |
| -DMALLOB_USE_JEMALLOC=<0/1>   | Compile with Address Sanitizer for debugging purposes.                                                     |
| -DMALLOB_LOG_VERBOSITY=<0..6> | Only compile logging messages of the provided maximum verbosity and discard more verbose log calls.        |

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
RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun <mpi-options> ./mallob <options>
```
If you use Mallob within Docker, replace `build/:$PATH` with `.:$PATH`.
Each option has the syntax `-key=value`.

To "daemonize" Mallob, i.e., to let it run in the background as a server for your own application(s), run
```
RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun <mpi-options> ./mallob <options> 2>&1 > OUT &
```
where `OUT` is a text file to create for Mallob's output. (If you do not want such a file, use the "quiet" option `-q` instead.)
If running in the background, do not forget to `kill` Mallob (i.e., SIGTERM the `mpirun` process) after you are done.
Alternatively you can specify the number of jobs to process (with `-J=<num-jobs>`) and/or the time to pass (with `-T=<timelim-secs>`) before Mallob should terminate on its own.

For exact and clean logging, you should not rely on a textfile in which you piped Mallob's output (like `OUT` above).
Instead, specify a logging directory with `-log=<log-dir>` where separate sub-directories and files will be created for each worker / thread.

### Solving a single SAT instance

Call Mallob with the option `-mono=<cnf-file>`.
Mallob will run in single instance solving mode on the provided CNF file: All available MPI processes (if any) are used with full power to resolve the formula in parallel. This option overrides a couple of options concerning balancing and job demands.

### Scheduling and solving many instances

Launch Mallob without any particular options regarding its mode of operation. Each PE of rank i then spins up a JSON client interface which can be used over the PE's file system under `<mallob base directory>/.api/jobs.<i>/`.
On a shared-memory machine, the easiest option is to just always use the directory `.api/jobs.0/` to introduce your jobs, leaving all other client interfaces idle.
In case of many concurrent jobs, it is best to distribute your jobs evenly (or uniformly @ random) across all existing client interfaces in order to minimize response times.

The number of worker PEs and client PEs can be set manually with the `-w=<#workers>` and `-c=<#clients>` options to enforce that some PEs are exclusively clients or exclusively workers.
The first `#workers` ranks are assigned worker roles, and the last `#clients` ranks are assigned client roles. These may overlap, and setting one/both of the options to -1 means that _all_ PEs are assigned the respective role(s). Use `-c=1` if you only use `.api/jobs.0/` in order to minimize overhead.

**Introducing a Job**

To introduce a job to the system, drop a JSON file in `.api/jobs.<i>/in/` (e.g., `.api/jobs.0/in/`) on the filesystem of the according PE structured like this:  
```
{ 
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
| application       | no        | String       | Which kind of problem is being solved; currently either of "SAT" or "DUMMY" (default: DUMMY)                   |
| wallclock-limit   | no        | String       | Job wallclock limit: combination of a number and a unit (ms/s/m/h/d)                                           |
| cpu-limit         | no        | String       | Job CPU time limit: combination of a number and a unit (ms/s/m/h/d)                                            |
| arrival           | no        | Float >= 0   | Job's arrival time (seconds) since program start; ignore job until then                                        |
| max-demand        | no        | Int >= 0     | Override the max. number of MPI processes this job should receive at any point in time (0: no limit)           |
| dependencies      | no        | String array | User-qualified job names (using "." as a separator) which must exit **before** this job is introduced          |
| content-mode      | no        | String       | If "raw", the input file will be read as a binary file and not as a text file.                                 |
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

**Retrieving a Job Result**

Upon completion of a job, Mallob writes a result JSON file under `.api/jobs.0/out/<user-name>.<job-name>.json` (you can repeatedly query the directory contents or employ a kernel-level mechanism like `inotify`).
Such a file may look like this:
```
{
    "cpu-limit": "10h",
    "file": "/path/to/difficult/formula.cnf",
    "name": "test-job-1",
    "priority": 0.7,
    "result": {
        "resultcode": 10,
        "resultstring": "SAT",
        "solution": [
            0,
            1,
            2,
            3,
            4,
            5
        ]
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
Instead of the "solution" field, the response may also contain the fields "solution-size" and "solution-file" if the solution is large. In that case, your application has to read `solution-size` integers (as bytes) representing the solution from the named pipe located at `solution-file`.

### Options

All command-line options of Mallob can be seen by executing Mallob with the `-h` option. This also works without the `mpirun` prefix.

<hr/>

## Programming Interfaces

Mallob can be extended in the following ways:

* New options to the application (or a subsystem thereof) can be added in `src/optionslist.hpp`.
* To add a new SAT solver to be used in a SAT solver engine, do the following:
    - Add a subclass of `PortfolioSolverInterface`. (You can use the existing implementation for any of the existing solvers and adapt it to your solver.)
    - Add your solver to the portfolio initialization in `src/app/sat/execution/engine.cpp`.
* To extend Mallob by adding another kind of application (like combinatorial search, planning, SMT, ...), do the following:
    - Extend the enumeration `JobDescription::Application` by a corresponding item.
    - In `JobReader::read`, add a parser for your application.
    - In `JobFileAdapter::handleNewJob`, add a new case for the `application` field in JSON files.
    - Create a subclass of `Job` (see `src/app/job.hpp`) and implement all pure virtual methods. 
    - Add an additional case to `JobDatabase::createJob`.
* To add a unit test, create a class `test_*.cpp` in `src/test` and then add the test case to the bottom of `CMakeLists.txt`.
* To add a system test, consult the files `scripts/systest_commons.sh` and/or `scripts/systest.sh`.

<hr/>

## Licensing

In its default configuration, the source code of Mallob can be used, changed and redistributed under the terms of the Lesser General Public License (LGPLv3), one notable exception being the source file `src/app/sat/hordesat/solvers/glucose.cpp` (see below).
The used versions of Lingeling, YalSAT, CaDiCaL, and Kissat are MIT-licensed, as is HordeSat.

The Glucose interface of Mallob, unfortunately, is non-free software due to the [non-free license of (parallel-ready) Glucose](https://github.com/mi-ki/glucose-syrup/blob/master/LICENCE). Notably, its usage in competitive events is restricted. So when compiling Mallob with `MALLOB_USE_GLUCOSE=1` make sure that you have read and understood these restrictions.

<hr/>

## Remarks

Many thanks to Armin Biere et al. for the SAT solvers Lingeling, YalSAT, CaDiCaL, and Kissat which this system uses by default, and to Tomáš Balyo for HordeSat, the portfolio solver this project's solver engine is built upon.

Furthermore, in our implementation we make thankful use of the following projects:

* [Compile Time Regular Expressions](https://github.com/hanickadot/compile-time-regular-expressions) by Hana Dusíková, for matching particular user inputs
* [robin_hood hashing](https://github.com/martinus/robin-hood-hashing) by Martin Ankerl, for efficient unordered maps and sets
* [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann, for reading and writing JSON files

If you make use of Mallob in an academic setting, please cite this SAT'21 conference paper:

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
