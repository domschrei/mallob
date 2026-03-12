
# Setting up Mallob

This page explains how to build and set up Mallob.

## Prerequisites

Note that we only support Linux as an operating system for actual distributed computing.
Some people have been developing and experimenting successfully with Mallob within the WSL and on IOS.

* CMake ≥ 3.11.4
* An MPI implementation including development files, e.g., Open MPI
* GDB
* [jemalloc](https://github.com/jemalloc/jemalloc)
    * optional but recommended - available in common package repositories like `apt`
    * If you use a non system level installation of jemalloc, you can use `-DMALLOB_JEMALLOC_DIR` (see below) to set the correct path.

## General Build Instructions

Mallob is built using CMake.
[`scripts/setup/build.sh`](../scripts/setup/build.sh) provides a default build script.
We repeat its commands here:

```bash
# Only needed if building with -DMALLOB_APP_SAT=1 (enabled by default).
# For non-x86-64 architectures (ARM, POWER9, etc.), prepend `export DISABLE_FPU=1;`.
scripts/setup/sat-setup.sh

# Only needed if building with -DMALLOB_APP_MAXSAT=1 and -DMALLOB_APP_SMT=1, respectively.
scripts/setup/maxsat-setup.sh
scripts/setup/smt-setup.sh

# Build Mallob. You can modify and/or append build options like -DMALLOB_APP_MAXSAT=1.
# Find all build options at: docs/setup.md
scripts/setup/cmake-make.sh build -DMALLOB_APP_MAXSAT=1 -DMALLOB_APP_SMT=1 -DMALLOB_APP_INCSAT=1 -DMALLOB_APP_SATWITHPRE=1 -DMALLOB_BUILD_LRAT_MODULES=1
```

In the main build call, you can use the following Mallob-specific build options:

| Usage                                       | Description                                                                                                |
| ------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| -DMALLOB_ASSERT=<0/1>                       | Turn on assertions (even on release builds). Setting to 0 limits assertions to debug builds.               |
| -DMALLOB_JEMALLOC_DIR=path                  | If necessary, provide a path to a local installation of `jemalloc` where `libjemalloc.*` is located.       |
| -DMALLOB_LOG_VERBOSITY=<0..6>               | Only compile logging messages of the provided maximum verbosity and discard more verbose log calls.        |
| -DMALLOB_SUBPROC_DISPATCH_PATH=\\"path\\"   | Subprocess executables must be located under <path> for Mallob to find. (Use `\"build/\"` by default.)     |
| -DMALLOB_USE_ASAN=<0/1>                     | Compile with Address Sanitizer for debugging purposes.                                                     |
| -DMALLOB_USE_GLUCOSE=<0/1>                  | Compile with support for Glucose SAT solver (disabled by default for licensing reasons, see below).        |
| -DMALLOB_USE_JEMALLOC=<0/1>                 | Compile with Scalable Memory Allocator `jemalloc` instead of default `malloc`.                             |
| -DMALLOB_APP_*=<0/1>                        | Compile with the according application.                                                                    |
| -DMALLOB_USE_MAXPRE=<0/1>                   | For MaxSAT: Include a library of the preprocessor MaxPRE (default=1)                                       |
| -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=<N>   | Max. number of application threads (solver threads for SAT) per process to support. (max: 128)             |
| -DMALLOB_BUILD_LRAT_MODULES=<0/1>           | Also build standalone LRAT checker                                                                         |

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
