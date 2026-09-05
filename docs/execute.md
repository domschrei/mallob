
# Executing Mallob

This page explains how to execute Mallob in general, with different applications, and in different modes of operation.

## General

### Starting Mallob

> [!IMPORTANT]  
> **Always make sure to execute Mallob and its wrapper run scripts from the home directory of the Mallob repository**, otherwise Mallob will not find critical executables in `build/` and **will not work correctly**.

**Quick Start:**

If you just want to use Mallob on a single, parallel machine, then the script `scripts/run/mallob_local.sh` automatically retrieves a suitable process+thread configuration of Mallob for your hardware that makes use of the entire machine. Useful presets can be applied by calling the scripts at `config/presets/`. Examples:

```bash
# SAT solving (default, simple setup)
scripts/run/mallob_local.sh -mono=instances/r3unsat_300.cnf
# SAT solving (SAT Competition 2026 winning configuration, with Satsuma)
scripts/run/mallob_local.sh $(config/presets/satcomp2026-quick) -mono=instances/r3unsat_300.cnf
# SAT solving (with real-time proof checking and assignment checking)
scripts/run/mallob_local.sh $(config/presets/satcomp2026-safe) -mono=instances/r3unsat_300.cnf
# SMT solving
scripts/run/mallob_local.sh -mono=path/to/problem.smt2 -mono-app=SMT
# MaxSAT solving
scripts/run/mallob_local.sh -mono=path/to/problem.wcnf -mono-app=MAXSAT
```

**General settings:**

Mallob is an MPI application and should therefore usually be executed via `mpirun`, `mpiexec` or something similar.

For example, given a single machine with two hardware threads per core, the following command executed in Mallob's base directory assigns one MPI process to each set of four physical cores (eight hardware threads) and then runs four solver threads on each MPI process.

```
RDMAV_FORK_SAFE=1; NPROCS="$(($(nproc)/8))"; mpirun -np $NPROCS --bind-to core --map-by ppr:${NPROCS}:node:pe=4 build/mallob -t=4 $MALLOB_OPTIONS
```

Given a machine with `$nthreads` cores (and twice the number of hardware threads), the following command spawns a single process with one solver thread per core (per hardware thread):

```
RDMAV_FORK_SAFE=1; mpirun -np 1 --bind-to core --map-by ppr:1:node:pe=$nthreads build/mallob -t=$nthreads $MALLOB_OPTIONS
RDMAV_FORK_SAFE=1; mpirun -np 1 --bind-to hwthread --map-by ppr:1:node:pe=$((2*$nthreads)) build/mallob -t=$((2*$nthreads)) $MALLOB_OPTIONS
```

In this case, only executing `build/mallob -t=$nthreads $MALLOB_OPTIONS` (without `mpirun` and MPI options) works as well.

For running Mallob on distributed clusters, please also consult [our quickstart guide for clusters](clusters.md) and, in particular, [our SLURM scripting setup](../scripts/slurm/README.md).

### Terminating Mallob

In some modes of operation, Mallob stops on its own, e.g., after an instance has been solved (see "Mono mode of operation" below).
You can always stop Mallob via Ctrl+C (interrupt signal) or by executing `killall mpirun` (or `killall build/mallob`). 
Alternatively, you can specify the number of jobs to process (with `-J=$NUM_JOBS`) and/or the time to pass (with `-T=$TIME_LIMIT_SECS`) before Mallob terminates on its own.

### Program options and output

You can find **all program options of Mallob** by executing Mallob with the `-h` option. (This also works without the `mpirun` prefix.)

For exact and clean logging, specify a logging directory with `-log=<log-dir>` where separate sub-directories and files will be created for each worker / thread. 
This can be combined with the `-q` option to suppress Mallob's output to STDOUT. 
Verbosity of logging can be set with the `-v` option (as long as Mallob was compiled with the respective verbosity or higher, see the `-DMALLOB_LOG_VERBOSITY` build option).

**Presets:** We provide a few useful Mallob configuration presets in the directory `scripts/presets/`. Example:
```bash
# SAT solving (SAT Competition 2026 winning configuration, with Satsuma)
scripts/run/mallob_local.sh $(scripts/presets/satcomp2026-quick.sh) -mono=instances/r3unsat_300.cnf
```
You can easily add your own presets by dropping an executable (script / program) under `scripts/presets/` that simply outputs the desired series of program options.

Command-line options in Mallob can be specified repeatedly, in which case the **final occurrence** of the option will override all previously specified values of the option. This can be useful, e.g., when combined with presets: You can set a certain preset and then append your own configuration, possibly overriding some of the preset's settings.

### Mono mode of operation

In order to let Mallob process only a single instance, use option `-mono=$PROBLEM_FILE` where `$PROBLEM_FILE` is the path and file name of the problem to solve. Specify the application of this instance with `-mono-app=APPKEY`, where APPKEY can be SAT, KMEANS, MAXSAT, etc.
In this mode, all processes participate in solving, overhead is minimal, and Mallob terminates immediately after the job has been processed.
Use option `-s2f=path/to/output.txt` ("solution to file") to write the result and (if applicable) the found satisfying assignment to a text file.

In terms of file inputs and outputs, note that you can also use UNIX named pipes to replace disk operations with direct inter-process communication: Create the input file(s) and/or the solution output file in advance via `mkfifo` and make sure to write/read the respective contents to/from the pipes concurrently so that no thread blocks indefinitely.

In the following, we explain how to use Mallob with different applications while mostly focusing on this "mono" mode of operation.
Afterwards, we explain Mallob's other modes of operation (solving multiple instances in an orchestrated manner, and processing jobs on demand).

## SAT Solving

For using and configuring Mallob's SAT solving, see the documentation on the [SAT engine (MallobSat)](app/sat/README.md) and [SATWITHPRE engine (MallobSat + Cascading Preprocessing)](app/satwithpre/README.md).

## MaxSAT Solving

Compile Mallob with `-DMALLOB_APP_MAXSAT=1` after building the MaxSAT dependencies as indicated above.  

Mallob expects `WCNF` instances and internally invokes `MaxPRE` to preprocess the instance and arrive at an objective-based formulation of the problem. 

Here is an example 16-core (4x4) invocation that runs MaxPRE for up to (roughly) 5 seconds, decides on the PB encoding heuristically (`-maxsat-card-encoding=3`), runs two searchers in parallel initially and deletes the "leftmost" searcher after 30s of stagnation.

```
export RDMAV_FORK_SAFE=1;
mpirun -np 4 --oversubscribe build/mallob -mono-app=MAXSAT -mono=instances/wcnf/warehouses_wt-warehouse0.wcsp.wcnf -v=4 -t=4 -satsolver=C -adc=1 -cjc=1 -pre-cleanup=1 -maxpre=1 -maxpre-timeout=5 -maxsat-card-encoding=3 -maxsat-searchers=2 -maxsat-focus-period=30 | grep -iE "maxsat|solution"
```

## SMT Solving

You can use Mallob for SMT solving on any logics/theories supported by the SMT solver [Bitwuzla](https://github.com/bitwuzla/bitwuzla).

Compile Mallob with `-DMALLOB_APP_SMT=1` after building the SMT dependencies (`cd lib && bash fetch_and_build_smt_deps.sh`).

Mallob expects `.smt2` instances and internally invokes Bitwuzla, which in turn uses Mallob's incremental SAT task interface as its SAT solving backend.

Here is an example 16-core (4x4) invocation that redirects Bitwuzla's result stream to `out.smt2` and ensures that it outputs any found models:

```
mpirun -np 4 build/mallob -mono-app=SMT -mono=instances/smt/modulus_true.c.17.smt2 -v=4 -t=4 -pre-cleanup=1 -smt-out-file=out.smt2 -smt-args=--print-model
```

## Solve multiple instances in an orchestrated manner

If you want to solve a fixed set of $n$ jobs or wish to evaluate Mallob's scheduling behavior with simulated jobs, follow these steps:

* Write the set of jobs into a text file `$INSTANCE_FILE` (one line per path).
* Configure the base properties of a job with a JSON file `$JOB_TEMPLATE`. For a plain job with default properties you can use `templates/job-template.json`.
* Configure the behavior of each job-introducing process ("client") with a JSON file `$CLIENT_TEMPLATE`. You can find the simplest possible configuration in `templates/client-template.json` and a more complex randomized configuration in `templates/client-template-random.json`. Both files contain all necessary documentation to adjust them as desired.

Then use these Mallob options:
```
-c=1 -ajpc=$MAX_PAR_JOBS -ljpc=$((2*$MAX_PAR_JOBS)) -J=$NUM_JOBS -job-desc-template=$INSTANCE_FILE -job-template=$JOB_TEMPLATE -client-template=$CLIENT_TEMPLATE -pls=0
```
where `$NUM_JOBS` is set to $n$ (if it is larger than $n$, a client cycles through the provided job descriptions indefinitely). You can set `-sjd=1` to shuffle the provided job descriptions. You can also increase the number of client processes introducing jobs by increasing the value of `-c`. However, note that the provided configuration for active jobs in the system is applied to each of the clients independently, hence the inputs provided in the instance file are not split up among the clients but rather duplicated.

## Process jobs on demand

This is the default and most general configuration of Mallob, i.e., without `-mono` or `-job-template` options.
You can manually set the number of worker processes (`-w`) and the number of client processes introducing jobs (`-c`). By default, all processes are workers (`-w=-1`) and a single process is additionally a client (`-c=1`). The $k$ client processes are the $k$ processes of the highest ranks, and they open up file system interfaces for introducing jobs and retrieving results at the directories `.api/jobs.0/` through `.api/jobs.`$k-1$`/`.

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
| files             | **yes***  | String array | File path(s) of the input(s) to solve                                                                          |
| priority          | **yes***  | Float > 0    | Priority of the job (higher is more important)                                                                 |
| application       | **yes**   | String       | Which kind of problem is being solved; SAT, SATWITHPRE, MAXSAT, DUMMY, ...                                     |
| wallclock-limit   | no        | String       | Job wallclock limit: combination of a number and a unit (ms/s/m/h/d)                                           |
| cpu-limit         | no        | String       | Job CPU time limit: combination of a number and a unit (ms/s/m/h/d)                                            |
| arrival           | no        | Float >= 0   | Job's arrival time (seconds) since program start; ignore job until then                                        |
| max-demand        | no        | Int >= 0     | Override the max. number of MPI processes this job should receive at any point in time (0: no limit)           |
| dependencies      | no        | String array | User-qualified job names (using "." as a separator) which must exit **before** this job is introduced          |
| interrupt         | no        | Bool         | If `true`, the job given by "user" and "name" is interrupted (for incremental jobs, just the current revision).|
| incremental       | no        | Bool         | Whether this job has multiple _increments_ / _revisions_ and should be treated as such                         |
| literals          | no        | Int array    | Instead of "files", you can alternatively specify the SAT formula (for this increment) directly in the JSON.   |
| precursor         | no        | String       | _(Only for incremental jobs)_ User-qualified job name (`<user>.<jobname>`) of this job's previous increment    |
| done              | no        | Bool         | _(Only for incremental jobs)_ If `true`, the incremental job given by "precursor" is finalized and cleaned up. |

*) Not needed if `done` is set to `true`.

In the above example, a job is introduced with priority 0.7, with a wallclock limit of five minutes and a CPU limit of 10 CPUh.

If you provide named pipes as special input files via `"files"`, make sure that (a) the named pipe is already created when submitting the job and (b) your application writes the formula to the pipe _after_ submitting the job (else it will hang indefinitely except if this is done in a separate thread).

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
The result code is 0 is unknown, 10 if SAT (solved successfully), and 20 if UNSAT (no solution exists).
The `solution` field is application-dependent.
For SAT solving, in case of SATISFIABLE, the solution field contains the found satisfying assignment; in case of UNSAT, the result for an incremental job contains the set of failed assumptions.
Instead of the "solution" field, the response may also contain the fields "solution-size" and "solution-file" if the solution is large and if option `-pls` is set. In that case, your application has to read `solution-size` integers (as bytes) representing the solution from the named pipe located at `solution-file`.

## Debugging

Please consult [develop.md -> Debugging Mallob](develop.md#debugging-mallob) for some notes on how Mallob runs can be diagnosed and debugged appropriately.
