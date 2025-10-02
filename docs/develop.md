
# Development with Mallob


## General

If you change or replace individual libraries under `lib/`, you'll need to rebuild parts of Mallob for it to reflect the changes.
We advise to just remove the Mallob binaries in the build directory (`rm build/*mallob*`) and then re-build with the usual commands.
This should then only recreate the missing binaries.

The following list shows a few examples for how Mallob can be extended:

* To add a new SAT solver to be used in a SAT solver engine, do the following:
    - Add a subclass of `PortfolioSolverInterface`. (You can use the existing implementation for any of the existing solvers and adapt it to your solver.)
    - Add your solver to the portfolio initialization in `src/app/sat/execution/engine.cpp`.
* To extend Mallob by adding another kind of application (e.g., automated planning, ILP, ...), find detailed instructions at [application_engines.md](application_engines.md).
* To add a unit test, create a class `test_*.cpp` in `src/test` and then add the test case to the bottom of `CMakeLists.txt`.
* To add a system test, see the file `scripts/systest.sh` (which includes definitions from `scripts/systest_commons.sh`).


## Important Interfaces

* Add new options for Mallob easily by extending `src/optionslist.hpp`.
    - Options which are specific to a certain application can be found and edited in `src/app/$APPKEY/options.hpp`.
* Use the `LOG()` macro to log messages. For thread-safe logging and clean outputs in the directory provided via `-log=`, each non main thread should have its own instance of `Logger` (obtained via `Logger::copy` from the parent thread's logger) and use the `LOGGER(logger, ...)` macro. (Note that this "safe" logging is not yet fully implemented across all modules of Mallob.)
* Use `ProcessWideThreadPool::addTask` or `BackgroundWorker` to perform background tasks in other threads. For multi-threading utilities, `src/util/sys/threading.hpp` features an easy-to-use `Mutex` class with RAII functionality as well as a corresponding `ConditionVariable` class and some utilities for `std::future` objects.
* Send messages across different Mallob MPI processes via `MyMpi::isend` for global (Mallob-level) messages and via `getJobTree().send*` for job-/application-specific messages. Receive messages by simply creating an RAII-style instance of `MessageSubscription` or `CondMessageSubscription` (_conditional_, if you need the option to "decline" an incoming message and leave it to some other module instead); the callback you provide in the constructor will be executed whenever an according message arrives. Accordingly extend `src/comm/msgtags.h` by your new, **fresh** message tags. For application-specific message passing, find some more information at [application_engines.md](application_engines.md).
Note that such communication **may only be performed by each MPI process' main thread**. For example, a callback into Mallob from within a solver-thread should not itself send out MPI messages, but store the request in a queue, which is then picked up shortly afterwards by the MPI process' main thread.


## Debugging Mallob

Debugging of distributed applications can be difficult, especially in Mallob's case where message passing goes hand in hand with multithreading and inter-process communication. Here are some notes on how Mallob runs can be diagnosed and debugged appropriately.

See also our [Frequently Asked Questions](faq.md) for some common issues.

### Build Options

For debugging purposes, the following build switches are useful:

* `-DCMAKE_BUILD_TYPE=DEBUG`: Produces a build with full debug information: In thread trace files and in the output of valgrind tools, there will be more specific information on functions and line numbers. Assertions are always included in DEBUG builds.
* `-DMALLOB_LOG_VERBOSITY=5`: This static verbosity level (together with run time option `-v=5`) logs every single message that is being sent or received, among many other things.
* `-DMALLOB_USE_ASAN=1`: Build sources with AddressSanitizer. This can help find illegal states in the code, especially invalid memory accesses.

### In-built Debugging Features

Get acquainted with the debugging features Mallob offers by itself.

#### Consulting Logs

In general, taking a quick look at the logs a run outputs is never wrong. For small to medium size runs, you can use this command to take a look at Mallob's entire output, sorted consistently, given a log directory LOG:

    cat LOG/*/* | awk 'NF > 1' | sort -s -g | less

If run Mallob with `-mono`, a `c ` is output at the beginning of each line, which you can ignore like this:

    cat LOG/*/* | awk 'NF > 1' | sed 's/^c //g' | sort -s -g | less

To further diagnose a Mallob run, `grep` in the log files for the following logging output (listed by descending priority):

* Log lines containing `[ERROR]` **do** indicate undesired behavior, such as an internal crash, an unresponsive node, or a failed assertion. Usually, the output of such a line is accompanied by a thread trace file (see next section).
* Log lines containing `[WARN]` _may_ indicate undesired behavior, such as the main thread getting stuck in a computation for some time or an unexpected message arriving at a process. Relevant warnings which should be investigated include watchdog barks (see below) and the rejection of an incoming job, perhaps because its JSON was malformed.
* Log lines containing `sysstate` should be output each second and provide basic information on the system state, such as the number of entered/parsed/introduced/finished jobs, the global memory usage, and the ratio of busy processes. If this `busyratio` is less than 1.000 for multiple seconds in a row, this usually means one of two things: Either not enough demand for resources is present in the system to utilize all processes, or (this is the bad option) something went wrong with Mallob's scheduling leading to some remaining idle processes. The latter case needs to be debugged separately, and if you notice this behavior without changing anything about Mallob's scheduling, please contact <dominik.schreiber@kit.edu>.

#### Thread Trace Files

Mallob processes have an in-built mechanism to catch and process signals like SIGSEGV, SIGBUS or SIGABRT which are thrown if something goes wrong (like an invalid memory access, no memory left, or a failed assertion). If such a signal is caught, the process will execute `gdb` on itself to retrieve the current location in the code from where the signal was thrown. The resulting stack trace is written to a file `mallob_thread_trace_of_TRACEDTID_by_TRACINGTID` where `TRACEDTID` is the thread ID of the thread being traced and `TRACINGID` is the thread ID of the thread tracing the other one. Consult these files if Mallob crashes! They usually contain helpful information on what might have gone wrong.

The directory where these files are written to can be changed with run time option `-trace-dir`.

#### Thread Naming

Each thread running in Mallob is usually given a name via `Proc::nameThisThread(name)`. This name can be seen in tools like `htop` to diagnose unusual behavior.

If, for instance, you observe a crash signal (SIGSEGV / SIGABRT / SIGPIPE / ...) and you are not sure where it's coming from (and trace files don't help for whichever reason), then you can name your own threads as well and then adjust `Proc::nameThisThread` to output each TID-to-name mapping. This allows you to convert the crashing TID to an actual role in the program.

### Watchdogs

Since Mallob as a platform is designed for latencies in the realm of milliseconds, it is essential that the threads which advance the scheduling – in particular the main thread – do not get stuck in a computation or some wait that takes several milliseconds. To diagnose such behavior, Mallob features a watchdog mechanism for selected threads and tasks: A separate thread (the "watchdog") is pet periodically by the watched thread. If such a pet does not occur for an extended period, the watchdog will begin barking to signal that something is not right. If this period gets too long, the watchdog triggers a crash of the program. 

A watchdog barking looks like this:

    11.814 7 [WARN] Watchdog: No reset for 62 ms (activity=0 recvtag=0 sendtag=0)

The `activity`, `recvtag`, and `sendtag` numbers are an indication of what the watched thread is currently doing. The activity IDs are defined in `src/util/sys/watchdog.hpp`. If a message is being handled, the according tags given as `recvtag` and `sendtag` are defined in `src/comm/msgtags.h`.

A watchdog triggering a crash looks like this:

    10.020 0 [ERROR] Watchdog: TIMEOUT (last=0.015 activity=0 recvtag=0 sendtag=0)

The value of `last` indicates the point in time where the watchdog was pet for the last time. In addition, a thread trace file is created which features the location in the code where the watched thread got stuck.

There are many possible causes for a barking watchdog. For instance, if you decide to assign each MPI process to a single core or even a single hardware thread, or if you use even more MPI processes with ``--oversubscribe``, then the machine is likely to experience very busy process / thread scheduling and it becomes more likely that a crucial thread is not scheduled for several milliseconds. Likewise, limited I/O bandwidth can become a problem if many processes write to the same (network?) file system.

If a certain thread hangs indeterminately and you don't know why, you can add your own watchdog like this: 
```c++
// Set up a watchdog
Watchdog watchdog(/*enabled?=*/true, /*checkIntervalMs=*/500, /*useThreadPool=*/true);
watchdog.setWarningPeriod(500); // print a warning after x*500ms (x>=1) without reset()
watchdog.setAbortPeriod(10'000); // trace the thread and abort() after 10s
// ... now do the computation which hangs ...
watchdog.reset(); // call reset() in between the heavy lifting where the thread can hang
// ... watchdog is RAII, so if it goes out of scope it will be joined properly
```

#### Detection of Unresponsive MPI Processes

If one or multiple MPI processes, perhaps on a different compute node, stop responding to the other processes, this will be recognized, and after 60s without a response the program will crash with this error message:

    61.024 9 [ERROR] Unresponsive node(s)

If you receive this message, it might be a node failure in your distributed system or the communication between processes not working correctly.

### Valgrind

Mallob processes can be executed via `valgrind` to detect errors or to profile performance or memory usage. In your call to Mallob, just replace 

    mpirun MPI_OPTIONS build/mallob MALLOB_OPTIONS

with

    mpirun MPI_OPTIONS valgrind VALGRIND_OPTIONS build/mallob MALLOB_OPTIONS

and each MPI process will be run through `valgrind`. The slowdown is very significant (maybe around a factor of 20-50) so this can only be done feasibly for small and short tasks.

A particularity for the application of SAT solving is that Mallob spawns separate sub-processes (as opposed to threads in the same process) which are, by default, not run through valgrind even if the MPI process is run through valgrind. This can be convenient since the processing of SAT jobs will still occur at a somewhat normal pace even if Mallob itself is run through vallgrind. However, you can use run time option `-subproc-prefix` to specify a program or a script which should be called with the actual subprocess call as its arguments, such as `scripts/run/run_as_valgrind.sh`.

You can also use `valgrind` for performance or memory profiling. Take a look at the scripts `scripts/run/run_as_callgrind.sh` and `scripts/run/run_as_massif.sh` (to be used via `-subproc-prefix`) for more information.
