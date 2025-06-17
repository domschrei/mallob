
# Executing Mallob

This page explains how to execute Mallob in general, with different applications, and in different modes of operation.

## General

### Starting Mallob

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

### Mono mode of operation

In order to let Mallob process only a single instance, use option `-mono=$PROBLEM_FILE` where `$PROBLEM_FILE` is the path and file name of the problem to solve. Specify the application of this instance with `-mono-app=APPKEY`, where APPKEY can be SAT, KMEANS, MAXSAT, etc.
In this mode, all processes participate in solving, overhead is minimal, and Mallob terminates immediately after the job has been processed.
Use option `-s2f=path/to/output.txt` ("solution to file") to write the result and (if applicable) the found satisfying assignment to a text file.

In the following, we explain how to use Mallob with different applications while mostly focusing on this "mono" mode of operation.
Afterwards, we explain Mallob's other modes of operation (solving multiple instances in an orchestrated manner, and processing jobs on demand).

## SAT Solving

For the application of (basic) SAT solving, Mallob expects a single input in DIMACS CNF format, possibly with .xz or .lzma compression.

### Producing Monolithic Proofs of Unsatisfiability

To enable proof production, just set the option `-proof=path/to/final/compressed/prooffile.lrat` together with `-mono=path/to/input.cnf`. You also need to set a log directory with `-proof-dir=path/to/dir` where intermediate files will be written to on each machine. The final proof is output in compressed LRAT format.

CaDiCaL is currently the only supported solver backend for parallel/distributed proof production. However, it is possible to employ other solvers as long as their only purpose is to find a satisfying assignment (i.e., exported clauses and unsatisfiability results from these solvers are discarded). See *Portfolio Tweaking* below.

You can check the output proof with the standalone LRAT checker that comes with Mallob if you set `-DMALLOB_BUILD_LRAT_MODULES=1` at build time:
```bash
build/standalone_lrat_checker path/to/input.cnf path/to/final/compressed/prooffile.lrat
```
Further synergies are possible; you can set `-uninvert=0` for Mallob and `--reversed` for the checker to avoid one entire I/O pass over the proof that "uninverts" its lines.
You can use the [`drat-trim`](https://github.com/marijnheule/drat-trim) tool suite to decompress a proof; note that you need to `#define MODE 2` (1=DRAT, 2=LRAT) in `decompress.c` before building.

### Real-time Proof Checking

Proof production can be costly and bottlenecked by the I/O bandwidth of the single process which needs to write the entire proof. A more scalable approach is to check all proof information on-the-fly, without writing it to disk, and to transfer clause soundness guarantees across machines via cryptographic signatures. This is explained in detail in our [2024 SAT publication](https://dominikschreiber.de/papers/2024-sat-trusted-pre.pdf).

Execute the command chain fetching and building [ImpCheck](https://github.com/domschrei/impcheck) in [`scripts/setup/build.sh`](../scripts/setup/build.sh). Then just use Mallob's option `-otfc=1` (without any `-proof*` options) to enable on-the-fly checking. Again, only CaDiCaL is supported for UNSAT whereas any solver can be employed for boosting satisfying assignments. By default, found satisfying assignments are also validated, which can be disabled via `-otfcm=0`.

Log lines of the following shape are reporting a trusted result from a proof checking process:
```
c 0.851 0 <#11606> S0.0 TRUSTED checker reported UNSAT - sig c6c0a823f35ce38cdb31c9483dc98143
c 0.851 0 <#11606> S0.0 TRUSTED checker reported SAT - sig a43e47d81715035d79290d1a6acf05e8
```
To be extra safe (e.g., if you are suspecting garbled or tampered-with logging output), execute the following command to validate the output signature:
```bash
build/impcheck_confirm -formula-input=path/to/input.cnf -result=X -result-sig=SIG
```
where `X` is either 10 (for SAT) or 20 (for UNSAT), and `SIG` is the reported signature.

**Note:** On-the-fly checking can also be used in Mallob's scheduled mode of operation. Globally unique clause IDs are ensured by adding a large offset times $x$ to a new solver thread's clause ID counter if the job has already experienced $x$ _balancing epochs_, i.e., received $x$ volume updates, since its initialization. The offset is chosen in such a way that 10,000 solvers each producing 10,000 clauses per second can run for 10,000 seconds before they may begin overlapping with clause IDs from the next balancing epoch. `ImpCheck` notices and reports any errors that would result from such a corner case.

### Portfolio Tweaking

Mallob allows to customize the employed SAT solver backends and some of their flavors. This is done with the `-satsolver` option, which expects a string representing the solver backends to cycle over. The option also allows to specify a "lasso word", i.e., a regex-like expression that consists of a finite prefix followed by an infinitely looping sequence. Here are some examples:
```bash
... -satsolver='c' # CaDiCaL only.
... -satsolver='kcl' # Kissat, CaDiCaL, Lingeling, Kissat, CaDiCaL, Lingeling, Kissat, ...
... -satsolver='k(c_)*' # One Kissat, then only plain (_) CaDiCaL. Always put brackets around the argument of '*'!
... -satsolver='kCLCLcl' # Capital letters indicate using truly incremental SAT solving for incremental jobs
... -satsolver='l+(c!){37}' # One Lingeling configured for satisfiable instances (+), then 37 LRAT-producing (!) CaDiCaLs, repeat
... -satsolver='(c!){37}k+((c!){37}l+)*' # As above, but replacing the 1st Lingeling with Kissat
... -satsolver='(c!){37}k+[[c!]{37}l+]w' # Alternative notation with squared brackets and automaton-style "omega" (w) to avoid issues with bash
```

## MaxSAT Solving

Compile Mallob with `-DMALLOB_APP_MAXSAT=1` after building the MaxSAT dependencies as indicated above.  

Mallob expects `WCNF` instances and internally invokes `MaxPRE` to preprocess the instance and arrive at an objective-based formulation of the problem. 

Here is an example 16-core (4x4) invocation that runs MaxPRE for up to (roughly) 5 seconds, decides on the PB encoding heuristically (`-maxsat-card-encoding=3`), runs two searchers in parallel initially and deletes the "leftmost" searcher after 30s of stagnation.

```
export RDMAV_FORK_SAFE=1;
mpirun -np 4 --oversubscribe build/mallob -mono-app=MAXSAT -mono=instances/wcnf/warehouses_wt-warehouse0.wcsp.wcnf -v=4 -t=4 -satsolver=C -adc=1 -cjc=1 -pre-cleanup=1 -maxpre=1 -maxpre-timeout=5 -maxsat-card-encoding=3 -maxsat-searchers=2 -maxsat-focus-period=30 | grep -iE "maxsat|solution"
```

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
The result code is 0 is unknown, 10 if SAT (solved successfully), and 20 if UNSAT (no solution exists).
The `solution` field is application-dependent.
For SAT solving, in case of SATISFIABLE, the solution field contains the found satisfying assignment; in case of UNSAT, the result for an incremental job contains the set of failed assumptions.
Instead of the "solution" field, the response may also contain the fields "solution-size" and "solution-file" if the solution is large and if option `-pls` is set. In that case, your application has to read `solution-size` integers (as bytes) representing the solution from the named pipe located at `solution-file`.

## Debugging

Please consult [develop.md -> Debugging Mallob](develop.md#debugging-mallob) for some notes on how Mallob runs can be diagnosed and debugged appropriately.
