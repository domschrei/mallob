
# Debugging Mallob

Debugging of distributed applications can be difficult, especially in Mallob's case where message passing goes hand in hand with multithreading and inter-process communication. Here are some notes on how Mallob runs can be diagnosed and debugged appropriately.

## Build Options

For debugging purposes, the following build switches are useful:

* `-DCMAKE_BUILD_TYPE=DEBUG`: Leads to a build with full debug information. Therefore, in thread trace files and in the output of valgrind tools, there will be more specific information on functions and line numbers. Assertions are always included in DEBUG builds.
* `-DMALLOB_LOG_VERBOSITY=5`: This static verbosity level (together with run time option `-v=5`) logs every single message that is being sent or received, among many other things.
* `-DMALLOB_USE_ASAN=1`: Build sources with AddressSanitizer. This can help find illegal states in the code, especially invalid memory accesses.

## In-built Debugging Features

Get acquainted with the debugging features Mallob offers by itself.

### Consulting Logs

In general, taking a quick look at the logs a run outputs is never wrong. For small to medium size runs, you can use this command to take a look at Mallob's entire output, sorted consistently, given a log directory LOG:

    cat LOG/*/* | awk 'NF > 1' | sort -s -g | less

If run Mallob with `-mono`, a `c ` is output at the beginning of each line, which you can ignore like this:

    cat LOG/*/* | awk 'NF > 1' | sed 's/^c //g' | sort -s -g | less

To further diagnose a Mallob run, `grep` in the log files for the following logging output (listed by descending priority):

* Log lines containing `[ERROR]` **do** indicate undesired behavior, such as an internal crash, an unresponsive node, or a failed assertion. Usually, the output of such a line is accompanied by a thread trace file (see next section).
* Log lines containing `[WARN]` _may_ indicate undesired behavior, such as the main thread getting stuck in a computation for some time or an unexpected message arriving at a process. Relevant warnings which should be investigated include watchdog barks (see below) and the rejection of an incoming job, perhaps because its JSON was malformed.
* Log lines containing `sysstate` should be output each second and provide basic information on the system state, such as the number of entered/parsed/introduced/finished jobs, the global memory usage, and the ratio of busy processes. If this `busyratio` is less than 1.000 for multiple seconds in a row, this usually means one of two things: Either not enough demand for resources is present in the system to utilize all processes, or (this is the bad option) something went wrong with Mallob's scheduling leading to some remaining idle processes. The latter case needs to be debugged separately, and if you notice this behavior without changing anything about Mallob's scheduling, please contact <dominik.schreiber@kit.edu>.

### Thread Trace Files

Mallob processes have an in-built mechanism to catch and process signals like SIGSEGV, SIGBUS or SIGABRT which are thrown if something goes wrong (like an invalid memory access, no memory left, or a failed assertion). If such a signal is caught, the process will execute `gdb` on itself to retrieve the current location in the code from where the signal was thrown. The resulting stack trace is written to a file `mallob_thread_trace_of_TRACEDTID_by_TRACINGTID` where `TRACEDTID` is the thread ID of the thread being traced and `TRACINGID` is the thread ID of the thread tracing the other one. Consult these files if Mallob crashes! They usually contain helpful information on what might have gone wrong.

The directory where these files are written to can be changed with run time option `-trace-dir`.

### Watchdogs

Since Mallob as a platform is designed for latencies in the realm of milliseconds, it is essential that the threads which advance the scheduling – in particular the main thread – do not get stuck in a computation or some wait that takes several milliseconds. To diagnose such behavior, Mallob features a watchdog mechanism for selected threads and tasks: A separate thread (the "watchdog") is pet periodically by the watched thread. If such a pet does not occur for an extended period, the watchdog will begin barking to signal that something is not right. If this period gets too long, the watchdog triggers a crash of the program. 

A watchdog barking looks like this:

    11.814 7 [WARN] Watchdog: No reset for 62 ms (activity=0 recvtag=0 sendtag=0)

The `activity`, `recvtag`, and `sendtag` numbers are an indication of what the watched thread is currently doing. The activity IDs are defined in `src/util/sys/watchdog.hpp`. If a message is being handled, the according tags given as `recvtag` and `sendtag` are defined in `src/comm/msgtags.h`.

A watchdog triggering a crash looks like this:

    10.020 0 [ERROR] Watchdog: TIMEOUT (last=0.015 activity=0 recvtag=0 sendtag=0)

The value of `last` indicates the point in time where the watchdog was pet for the last time. In addition, a thread trace file is created which features the location in the code where the watched thread got stuck.

There are many possible causes for a barking watchdog. For instance, if you decide to assign each MPI process to a single core or even a single hardware thread, or if you use even more MPI processes with ``--oversubscribe``, then the machine is likely to experience very busy process / thread scheduling and it becomes more likely that a crucial thread is not scheduled for several milliseconds. Likewise, limited I/O bandwidth can become a problem if many processes write to the same (network?) file system.

### Detection of Unresponsive MPI Processes

If one or multiple MPI processes, perhaps on a different compute node, stop responding to the other processes, this will be recognized, and after 60s without a response the program will crash with this error message:

    61.024 9 [ERROR] Unresponsive node(s)

If you receive this message, it might be a node failure in your distributed system or the communication between processes not working correctly.

## Valgrind

Mallob processes can be executed via `valgrind` to detect errors or to profile performance or memory usage. In your call to Mallob, just replace 

    mpirun MPI_OPTIONS build/mallob MALLOB_OPTIONS

with

    mpirun MPI_OPTIONS valgrind VALGRIND_OPTIONS build/mallob MALLOB_OPTIONS

and each MPI process will be run through `valgrind`. The slowdown is very significant (maybe around a factor of 20-50) so this can only be done feasibly for small and short tasks.

A particularity for the application of SAT solving is that Mallob spawns separate sub-processes (as opposed to threads in the same process) which are, by default, not run through valgrind even if the MPI process is run through valgrind. This can be convenient since the processing of SAT jobs will still occur at a somewhat normal pace even if Mallob itself is run through vallgrind. However, you can use run time option `-subproc-prefix` to specify a program or a script which should be called with the actual subprocess call as its arguments, such as `scripts/run/run_as_valgrind.sh`.

You can also use `valgrind` for performance or memory profiling. Take a look at the scripts `scripts/run/run_as_callgrind.sh` and `scripts/run/run_as_massif.sh` (to be used via `-subproc-prefix`) for more information.
