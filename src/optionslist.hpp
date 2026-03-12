
#ifndef DOMPASCH_MALLOB_OPTIONS_LIST_HPP
#define DOMPASCH_MALLOB_OPTIONS_LIST_HPP

#include "util/option.hpp"

#ifndef MALLOB_MAX_N_APPTHREADS_PER_PROCESS
#define MALLOB_MAX_N_APPTHREADS_PER_PROCESS 32
#endif

#define MALLOB_PIPE_SOLUTIONS_NONE 0
#define MALLOB_PIPE_SOLUTIONS_LARGE 1
#define MALLOB_PIPE_SOLUTIONS_ALL 2

// All declared options will be stored in this member of the Parameters class.
OptMap _global_map;
GroupedOptionsList _grouped_list;

// New options can be added here. They will then be initialized as member fields in the Parameters class.
// The value of an option can be queried on a Parameters object _params as such:
// _params.optionmember()
// The columns for OPT_* are defined as follows:
// TYPE  member name                      option ID (short, long)                      default (, min, max)     description

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpGeneral, "general", "General")
 OPT_BOOL(help,                           "h", "help",                                 false,                   "Print help and exit")
 OPT_STRING(monoFilename,                 "mono", "",                                  "",                      "Mono instance: Solve the provided CNF instance with full power, then exit") //[[AUTOCOMPLETE_FILE]]
 OPT_STRING(monoApplication,              "mono-app", "mono-application",              "SAT",                   "Application assumed for mono mode")
 OPT_INT(monoIncrements,                "mono-incs", "mono-increments", 0, 0, MAX_INT, "Whether the \"mono\" job should be considered incremental")
 OPT_INT(numJobs,                         "J", "jobs",                                 0,    0, LARGE_INT,      "Exit as soon as this number of jobs has been processed (set to 1 if -mono is used)")
 OPT_INT(numSuccessfulJobs,               "SJ", "successful-jobs",                     0,    0, LARGE_INT,      "Exit as soon as this number of jobs has been processed SUCCESSFULLY (i.e., not cancelled / aborted)")
 OPT_INT(seed,                            "seed", "",                                  0,    0, MAX_INT,        "Random seed")
 OPT_FLOAT(timeLimit,                     "T", "time-limit",                           0,    0, LARGE_INT,      "Run entire system for at most this many seconds")
 OPT_BOOL(warmup,                         "warmup", "",                                false,                   "Do one explicit All-To-All warmup among all nodes in the beginning")
 OPT_INT(numClients,                      "c", "clients",                              1,    -1, LARGE_INT,     "Number of client PEs to initialize (counting backwards from last rank). -1: all PEs are clients")
 OPT_INT(numWorkers,                      "w", "workers",                              -1,   -1, LARGE_INT,     "Number of worker PEs to initialize (beginning from rank #0), -1: all PEs are workers")
 OPT_BOOL(preCleanup,                     "pre-cleanup", "",                           false,                   "Clean up shmem/proof/trace/extmem files before starting off")
 OPT_STRING(clientApplication,            "client-app", "",                            "", "Have clients run specified external program path + \".\" + i for increasing i (or, if nonexistent, just the path as is)")
 OPT_INT(clientAppThreads,                "client-app-threads", "",                    1,    1, LARGE_INT,      "Max. number of applications to run per client")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpInterface, "interface", "Interface")
 OPT_INT(activeJobsPerClient,             "ajpc", "active-jobs-per-client",            0,         0, LARGE_INT, "Make each client have up to this many active jobs at any given time")
 OPT_STRING(clientTemplate,               "client-template", "",                       "",                      "JSON template file which each client uses to decide on job parameters (with -job-template option)") //[[AUTOCOMPLETE_FILE]]
 OPT_STRING(apiDirectory,                 "apidir", "api-dir",                         ".api",                  "Directory for filesystem API")
 OPT_INT(firstApiIndex,                   "fapii", "first-api-index",                  0,    0, LARGE_INT,      "1st API index: with c clients, uses [-apidir]/jobs.{<index>..<index>+c-1}/ as directories")
 OPT_BOOL(inotify,                        "inotify", "",                               true,                    "Use inotify for filesystem interface (otherwise, use naive directory polling)")
 OPT_STRING(jobDescriptionTemplate,       "job-desc-template", "",                     "",                      "Plain text file, one file path per line, to use as job descriptions (with -job-template option)") //[[AUTOCOMPLETE_FILE]]
 OPT_STRING(jobTemplate,                  "job-template", "",                          "",                      "JSON template file which each client uses to instantiate jobs indeterminately") //[[AUTOCOMPLETE_FILE]]
 OPT_INT(loadedJobsPerClient,             "ljpc", "loaded-jobs-per-client",            32,   0, LARGE_INT,      "Limit for how many job descriptions each client is allowed to have loaded at the same time")
 OPT_INT(maxJobsPerStreamer,              "mjps", "max-jobs-per-streamer",             0,    0, LARGE_INT,      "Maximum number of jobs to introduce per streamer")
 OPT_BOOL(shuffleJobDescriptions,         "sjd", "shuffle-job-descriptions",           false,                   "Shuffle job descriptions given via -job-desc-template option")
 OPT_BOOL(useFilesystemInterface,         "interface-fs", "",                          true,                    "Use filesystem interface ([-apidir]/jobs.*/{in,out}/*.json)")
 OPT_BOOL(useIPCSocketInterface,          "interface-ipc", "",                         false,                   "Use IPC socket interface (.mallob.<pid>.sk)")
 OPT_STRING(streamerResultOutput,         "sro", "streamer-result-output",             "",                      "Path for streamer to write result metadata to")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpOutput, "output", "Output")
 OPT_BOOL(coloredOutput,                  "colors", "",                                false,                   "Colored terminal output based on messages' verbosity")
 OPT_BOOL(immediateFileFlush,             "iff", "immediate-file-flush",               false,                   "Flush log files after each line instead of buffering")
 OPT_STRING(logDirectory,                 "log", "log-directory",                      "",                      "Directory to save logs in") //[[AUTOCOMPLETE_DIRECTORY]]
 OPT_BOOL(omitSolution,                   "os", "omit-solution",                       false,                   "Do not output solution in mono mode of operation")
 OPT_INT(pipeSolutions,                   "ps", "pipe-solutions",                      MALLOB_PIPE_SOLUTIONS_NONE, MALLOB_PIPE_SOLUTIONS_NONE, MALLOB_PIPE_SOLUTIONS_ALL,                   "Provide [0=no,1=large,2=all] solutions over a named pipe instead of directly writing them into the response JSON")
 OPT_BOOL(quiet,                          "q", "quiet",                                false,                   "Do not log to stdout besides critical information")
 OPT_STRING(solutionToFile,               "s2f", "solution-to-file",                   "",                      "Write solutions to file with provided base name + job ID")
 OPT_INT(verbosity,                       "v", "verbosity",                            2,    0, 6,              "Logging verbosity: 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB 5=DEBG")
 OPT_BOOL(zeroOnlyLogging,                "0o", "zero-only-logging",                   false,                   "Only PE of rank zero does logging")
 OPT_STRING(tmpDirectory,                 "tmp", "tmp-directory",                      "/tmp",                  "General temporary directory to write working files into")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpScheduling, "scheduling", "Scheduling")
 OPT_FLOAT(balancingPeriod,               "p", "balancing-period",                     0.1,  0, LARGE_INT,      "Minimum interval between subsequent rounds of balancing")
 OPT_BOOL(explicitVolumeUpdates,          "evu", "explicit-volume-updates",            false,                   "Broadcast volume updates through job tree instead of letting each PE compute it itself")
 OPT_INT(jobCacheSize,                    "jc", "job-cache-size",                      4,    0, LARGE_INT,      "Size of job cache per PE for suspended yet unfinished job nodes")
 OPT_FLOAT(loadFactor,                    "l", "load-factor",                          1,    0, 1,              "The share of PEs which should be busy at any given time")
 OPT_INT(numBounceAlternatives,           "ba", "bounce-alternatives",                 4,    1, LARGE_INT,      "Number of bounce alternatives per PE")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpSchedulingMapping, "scheduling/mapping", "Options for mapping workers to PEs")
 OPT_INT(hopsUntilCollectiveAssignment,   "huca", "hops-until-collective-assignment",  0,    -1, LARGE_INT,     "After a job request hopped this many times, add it to collective negotiation of requests and idle nodes (0: immediately, -1: never");
 OPT_BOOL(reactivationScheduling,         "rs", "use-reactivation-scheduling",         true,                    "Perform reactivation-based scheduling")
 OPT_BOOL(useDormantChildren,             "dc", "dormant-children",                    false,                   "Simple strategy of maintaining local set of dormant child job contexts which the parent tries to reactivate")
 OPT_BOOL(prefixSumMatching,              "prisma", "prefix-sum-matching",             false,                   "Match requests and idle PEs using prefix sums instead of a routing tree")
 OPT_BOOL(bulkRequests,                   "br", "bulk-requests",                       false,                   "Encode requests for an entire subtree as a single request")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpJob, "job", "Global configuration of jobs")
 OPT_FLOAT(appCommPeriod,                 "s", "app-comm-period",                      0.5,    0, LARGE_INT,      "Do job-internal communication every t seconds")
 OPT_STRING(applicationConfiguration,     "app-config", "",                            "",                      "Application configuration: structured as (-key=value;)*")
 OPT_BOOL(continuousGrowth,               "cg", "continuous-growth",                   true,                    "Continuous growth of job demands")
 OPT_FLOAT(growthPeriod,                  "g", "growth-period",                        0,    0, LARGE_INT,      "Grow job demand exponentially every t seconds (0: immediate full growth)" )
 OPT_BOOL(jitterJobPriorities,            "jjp", "jitter-job-priorities",              false,                   "Jitter job priorities to break ties during rebalancing")
 OPT_FLOAT(jobCommUpdatePeriod,           "jcup", "job-comm-update-period",            0,    0, LARGE_INT,      "Job communicator update period (0: never update)" )
 OPT_FLOAT(jobCpuLimit,                   "jcl", "job-cpu-limit",                      0,    0, LARGE_INT,      "Timeout an instance after x cpu seconds")
 OPT_FLOAT(jobWallclockLimit,             "jwl", "job-wallclock-limit",                0,    0, LARGE_INT,      "Timeout an instance after x seconds wall clock time")
 OPT_INT(maxDemand,                       "md", "max-demand",                          0,    0, LARGE_INT,      "Limit any job's demand to this value")
 OPT_INT(numThreadsPerProcess,            "t", "threads-per-process",                  1,    1, MALLOB_MAX_N_APPTHREADS_PER_PROCESS,
    "Number of application worker threads per MPI process; maximum value configurable at compile time via -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS")
 OPT_BOOL(aggressiveDescriptionCaching, "adc", "aggressive-desc-caching", false, "Try to reuse cached job descriptions by only transferring them when not repairable without them")
 OPT_BOOL(crossJobCommunication, "cjc", "cross-job-communication", false, "Enable communication across jobs, such as cross-problem clause sharing, within user-specified job groups")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpApp, "app", "Application-specific options")
// All application-specific options are included here.
// The included file is generated by CMake and includes all src/app/*/options.hpp
// for which the according application is included in the build.
#include "app/.register_options.h"

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpPerformance, "performance", "Performance")
 OPT_BOOL(memoryPanic,                    "mempanic", "",                              true,                    "Monitor RAM usage per physical machine and switch to memory panic mode if necessary")
 OPT_INT(messageBatchingThreshold,        "mbt", "message-batching-threshold",         8388608, 1000, MAX_INT,  "Employ batching of messages in batches of provided size")
 OPT_INT(processesPerHost,                "pph", "processes-per-host",                 0,    0, LARGE_INT,      "Tells Mallob how many MPI processes are executed on each physical host")
 OPT_BOOL(regularProcessDistribution,     "rpa", "regular-process-allocation",         false,                   "Signal that processes have been allocated regularly, i.e., the i-th machine hosts ranks c*i through c*i + c-1")
 OPT_INT(sleepMicrosecs,                  "sleep", "",                                 100,  0, LARGE_INT,      "Sleep this many microseconds between loop cycles of worker main thread")
 OPT_BOOL(yield,                          "yield", "",                                 false,                   "Yield manager thread whenever there are no new messages")
 OPT_INT(maxLiteralsPerThread,              "mlpt", "max-lits-per-thread",               50000000, 0,   MAX_INT,    
    "If formula is larger than threshold, reduce #threads per PE until #threads=1 or until limit is met \"on average\"")

///////////////////////////////////////////////////////////////////////

OPTION_GROUP(grpDebug, "debug", "Debugging")
 OPT_FLOAT(crashMonkeyProbability,        "cmp", "crash-monkey",                       0,    0, 1,              "Have an application thread crash with this probability each time it performs a certain action")
 OPT_BOOL(delayMonkey,                    "delaymonkey", "",                           false,                   "Small chance for each MPI call to block for some random amount of time")
 OPT_BOOL(latencyMonkey,                  "latencymonkey", "",                         false,                   "Block all MPI_Isend operations by a small randomized amount of time")
 OPT_BOOL(monitorMpi,                     "mmpi", "monitor-mpi",                       false,                   "Launch an additional thread per process checking when the main thread is inside an MPI call")
 OPT_STRING(subprocessPrefix,             "subproc-prefix", "",                        "",                      "Execute subprocesses with this prefix (e.g., \"valgrind\")")
 OPT_FLOAT(sysstatePeriod,                "y", "sysstate-period",                      1,    0.1, 50,           "Period for aggregating and logging global system state")
 OPT_STRING(traceDirectory,               "trace-dir", "",                             ".",                     "Directory to write thread trace files to") //[[AUTOCOMPLETE_DIRECTORY]]
 OPT_BOOL(useChecksums,                   "checksums", "",                             false,                   "Compute and verify checksum for every job description transfer")
 OPT_BOOL(watchdog,                       "watchdog", "",                              true,                    "Employ watchdog threads to detect unresponsive program flow")
 OPT_INT(watchdogAbortMillis,             "wam", "watchdog-abort-millis",              10000, 1, MAX_INT,       "Interval (in milliseconds) after which an un-reset watchdog in a worker's main thread will invoke a crash")

///////////////////////////////////////////////////////////////////////

OPT_BOOL(deterministicSolving,           "ds", "deterministic-solving",                          false,                       "Perform deterministic solving (only with -mono) - considerably slower!")

#endif
