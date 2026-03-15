
# Setting up Mallob

This page explains how to build and set up Mallob.

## Prerequisites

Note that we only support Linux as an operating system for actual distributed computing.
Some people have been developing and experimenting successfully with Mallob within the WSL and on IOS.

* CMake ≥ 3.11.4
* An MPI implementation including development files, e.g., Open MPI
* GDB

For a full list of system level dependencies, see the packages installed in [Mallob's Dockerfile](../docker/Dockerfile).

## General Build Instructions

Mallob is built using CMake.
The default build command is `bash scripts/setup/cmake-make.sh build` (see [here](../scripts/setup/cmake-make.sh)), which fetches all dependencies in `lib/` and creates a build of Mallob in `build/`.

By default, the above call creates a build that includes the most common SAT solving functionalities of Mallob. You can append the following arguments to the call to customize your build:

* `-DMALLOB_APP_<app>` for `<app>` = DUMMY, SAT, INCSAT, KMEANS, MAXSAT, SMT, PALRUPCHECK, SATCNC, SATWITHPRE
    - This includes or excludes specific application engines in/from your Mallob build. Some applications depend on each another (e.g., most depend on SAT) and the building script will raise an error if a dependency is not present.
* `-DMALLOB_BUILD_<mod>` for `<mod>` = IMPCHECK, CHECKER
    - Build certain standalone executables for use together with Mallob: IMPCHECK for the ImpCheck real-time proof checking suite (in two versions, incremental and verified) and CHECKER for a standalone efficient LRUP checker.
* `-DMALLOB_USE_<dep>` for `<dep>` = ASAN, JEMALLOC, MINISAT, CADICAL, LINGELING, KISSAT, RUSTSAT, MAXPRE
    - Include or exclude certain internal dependencies from linkage into Mallob. This concerns AdressSanitizer (ASAN) for debugging, JEMALLOC for more scalable memory allocation (enabled by default), and various SAT and MaxSAT backends (which are enabled by default for their respective application).
* `-DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=<t>` for `<t>` = 32, 64, (128)
    - Setting this as low as possible allows the SAT solving process to save a little bit of memory for each clause.
        - Note: Values beyond 64 are not recommended since too many solver threads per process might lead to performance issues; spawn more MPI processes with fewer solver threads instead. 
* `-DMALLOB_LOG_VERBOSITY=<v>` for `<v>` = 0, ..., 6
    - 0 means absolutely no logging except for critical output; 6 compiles _all_ logging calls into Mallob. Note that you still need to set the Mallob program option -v to an according value to actually see the respective log messages. The default level is 4.

## Testing

**Note:** By default, the test suite expects that Mallob is built and run with OpenMPI, i.e., that `mpirun` links to an OpenMPI executable on your system. For deviating MPI implementations, e.g., MPICH, try to prepend `mpiimpl=mpich` to the call `bash scripts/run/systest_commons.sh`.

In order to test that the system has been built and set up correctly, run the following command.
```
bash scripts/run/systest.sh mono drysched sched osc
```
This will locally run a suite of automated tests which cover the basic functionality of Mallob as a scheduler and as a SAT solving engine. 
To include Glucose in the tests, prepend the above command with "GLUCOSE=1".
Running the tests takes a few minutes and in the end "All tests done." should be output.
In case of problems, you can consult [develop.md -> Debugging Mallob](develop.md#debugging-mallob) for some notes on how Mallob runs can be diagnosed and debugged appropriately.
You can also prepend `nocleanup=1` to the call of the script in order to keep all log and trace files so that you can examine them.
