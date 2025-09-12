
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSat, "app/sat", "SAT solving options")
 OPT_BOOL(abortNonincrementalSubprocess,    "ans", "abort-noninc-subproc",               false,                   
    "Abort (hence restart) each sub-process which works (partially) non-incrementally upon the arrival of a new revision")
 OPT_BOOL(restartSubprocessAtAbort,         "rspaa", "restart-subproc-at-abort", false, "Ignore abort() of a subprocess and just restart it rather than aborting yourself")
 OPT_STRING(satEngineConfig,                "sec", "sat-engine-config",                  "",                      
    "Supply config for SAT engine subprocess [internal option, do not use]")
 OPT_BOOL(copyFormulaeFromSharedMem,        "cpshm", "",                                           false,
    "Copy each formula + assumptions from shared memory to local memory before launching solvers")
 OPT_STRING(clauseLog,                      "clause-log", "",                            "",
    "Log successfully shared clauses to the provided path")
 OPT_STRING(satProfilingDir,            "spd", "sat-profiling-dir", "", "Directory to write SAT thread profiling reports to")
 OPT_INT(satProfilingLevel,             "spl", "sat-profiling-level", -1, -1, 4, "Profiling level for SAT solvers (-1=none ... 4=all)")
 OPT_BOOL(compressFormula,                  "cf", "compress-formula", false, "Compress formula serialization (reorders clauses and literals in clauses)")
 OPT_BOOL(compressModels,                   "cm", "compress-models", false, "Compress found models into hexadecimal vector in output")
 OPT_STRING(groundTruthModel,               "gtm", "", "", "Ground truth model to test learned clauses against")
 OPT_INT(replay, "replay", "", 0, 0, 2, "0: nothing, 1: record solver threads' behavior, 2: replay solving")
 OPT_BOOL(internalStreamProcessor, "isp", "", true, "For incremental SAT job streams, run a local single-threaded SAT solver for latency hiding")

OPTION_GROUP(grpAppSatSharing, "app/sat/sharing", "Clause sharing configuration")
 OPT_INT(bufferedImportedClsGenerations,    "bicg", "buffered-imported-cls-generations", 4,        1,   LARGE_INT, 
    "Number of subsequent full clause sharings to fit in each solver's import buffer")
 OPT_INT(clauseBufferBaseSize,              "cbbs", "clause-buffer-base-size",           0,     0,   0,   
    "DISCONTINUED - use exportVolumePerThread (-evpt) instead, which is supposed to have a one-size-fits-all default")
 OPT_INT(exportVolumePerThread,             "evpt", "export-volume-per-thread",          500,   0,   LARGE_INT,
    "Max. number of clause literals to export per thread per sharing operation")
 OPT_INT(clauseBufferLimitMode,             "cblm", "clause-buffer-limit-mode",          1,        0,   1,
    "Mode for computing clause buffer limit w.r.t. worker count (0: unlimited growth based on levels of binary tree, 1: limited growth based on exponential function")
 OPT_FLOAT(clauseBufferLimitParam,          "cblp", "clause-buffer-limit-param",         100'000,  0,   MAX_INT,
    "Clause buffer discount factor: reduce buffer size per PE by <factor> each depth")
 OPT_FLOAT(clauseFilterClearInterval,       "cfci", "clause-filter-clear-interval",      10,       -1,  LARGE_INT,
    "Set clear interval of clauses in solver filters (-1: never clear, 0: always clear")
 OPT_BOOL(collectClauseHistory,           "ch", "collect-clause-history",                false,
    "Employ clause history collection mechanism")
 OPT_BOOL(compensateUnusedSharingVolume,    "cusv", "compensate-unused-sharing-volume",  true,
    "Compensate for unused or filtered parts of clause buffer in the next sharings")
 OPT_INT(freeClauseLengthLimit, "fcll", "free-clause-length-limit", 1, 0, LARGE_INT, "Max. length of clauses which are considered \"free\" for sharing")
 OPT_BOOL(groupClausesByLengthLbdSum,       "gclls", "group-by-length-lbd-sum",          false,                   
    "Group and prioritize clauses in buffers by the sum of clause length and LBD score")
 OPT_INT(maxLbdPartitioningSize,            "mlbdps", "max-lbd-partition-size",          2,        1,   LARGE_INT,
    "Store clauses with up to this LBD in separate buckets")
 OPT_INT(minNumChunksForImportPerSolver,    "mcips", "min-import-chunks-per-solver",     10,       1,   LARGE_INT,
    "Min. number of single-export-sized chunks for buffering incoming clauses for import per solver")
 OPT_INT(numExportChunks,                   "nec", "export-chunks",                      10,       1,   LARGE_INT,
    "Number of single-export-sized chunks for buffering produced clauses for export")
 OPT_INT(qualityClauseLengthLimit,          "qcll", "quality-clause-length-limit",       60,        0,   255,
    "Clauses up to this length are considered \"high quality\"")
 OPT_INT(qualityLbdLimit,                   "qlbdl", "quality-lbd-limit",                60,        0,   255,
    "Clauses with an LBD score up to this value are considered \"high quality\"")
 OPT_INT(clauseFilterMode,                  "cfm", "clause-filter-mode",                 3,        0,   3, 
    "0 = no filtering, 1 = bloom filters, 2 = exact filters, 3 = exact filters with distributed filtering in a 2nd all-reduction")
 OPT_INT(clauseStoreMode,                   "csm", "clause-store-mode",                  3,        -1,  3,
    "-1 = static by length w/ mixed LBD, 0 = static by length, 1 = static by LBD, 2 = adaptive by length + -mlbdps option, 3 = simplified adaptive")
 OPT_BOOL(lbdPriorityInner, "lbdpi", "lbd-priority-inner", false, "Whether LBD should be used as primary quality metric in the inner buckets (bound by \"quality\" limits)")
 OPT_BOOL(lbdPriorityOuter, "lbdpo", "lbd-priority-outer", false, "Whether LBD should be used as primary quality metric in the outer buckets (bound by \"strict\" limits)")
 OPT_INT(resetLbd,                          "rlbd", "reset-lbd"          ,                3,        0,   3,
    "Reset each clause's LBD to its length 0=never; 1=at import; 2=at export; 3=at production")
 OPT_INT(strictClauseLengthLimit,           "scll", "strict-clause-length-limit",        60,       0,   255,
    "Only clauses up to this length will be shared")
 OPT_INT(strictLbdLimit,                    "slbdl", "strict-lbd-limit",                 60,       0,   255,
    "Only clauses with an LBD score up to this value will be shared")
 OPT_BOOL(skipClauseSharingDiagonally,      "scsd", "skip-clause-sharing-diagonally",    false, "In the ith diversification round, disable clause sharing for the (i%%numDivs)th solver")
 OPT_FLOAT(maxSharingCompensationFactor,    "mscf", "max-sharing-compensation-factor",   5,        1,   LARGE_INT,
    "Max. relative increase in size of clause sharing buffers in case of many clauses being filtered")
 OPT_BOOL(backlogExportManager,             "bem", "backlog-export-manager",             true, "Use sequentialized export manager with backlogs instead of simple HordeSat-style export")
 OPT_BOOL(adaptiveImportManager,            "aim", "adaptive-import-manager",            true, "Use adaptive clause store for each solver's import buffer instead of lock-free ring buffers")
 OPT_BOOL(incrementLbd,                     "ilbd", "increment-lbd-at-import",           false, "Increment LBD value of each clause before import")
  OPT_INT(randomizeLbd,                     "randlbd", "randomize-lbd-at-import",        0,       0,      2, "Randomize the LBD value of each clause before import. 0=Never. 1=Uniformly. 2=Triangle-distribution. - can be combined with -ilbd afterwards")
 OPT_BOOL(noImport,                         "no-import", "",                             false, "Turn off solvers importing clauses (for comparison purposes)")
 OPT_BOOL(scrambleLbdScores,                "scramble-lbds", "",                         false, "For each clause length, randomly reassign the present LBD values to the present shared clauses")
 OPT_BOOL(priorityBasedBufferMerging, "pbbm", "priority-based-buffer-merging", false, "Use a more sophisticated and expensive merge procedure that adopts the prioritization of csm=3")
 OPT_INT(incrementalVariableDomainHeuristic, "ivdh", "incremental-variable-domain-heuristic", 1, 0, 2,
   ">=1: Replace LBD values with a rating based on how many clause literals are in the original (0th increment) variable range; 1=for cross-sharing only, 2=always. "
   ">=1 also overrides -lbdpi=1 -lbdpo=1 -pbbm=1 for cross-sharing ONLY.")

OPTION_GROUP(grpAppSatDiversification, "app/sat/diversification", "Diversification options")
 OPT_INT(diversifyElimination,              "div-elim", "",                              0,        0,   3,
    "0=normal diversification, 1/2/3=disable some/most/all variable elimination")
 OPT_BOOL(diversifyFanOut,                  "div-fanout", "",                            false,
    "Diversify some solvers based on making X random variable decisions every Y conflicts")
 OPT_BOOL(diversifyInitShuffle,             "div-init-shuffle", "",                      false, "Shuffle order in which variables are activated (->scored) initially")
 OPT_BOOL(diversifyPhases,                  "div-phases", "",                            true,
    "Diversify solvers based on random sparse variable phases in addition to native diversification")
 OPT_BOOL(diversifyNative,                 "div-native",  "",                            true,
    "Diversify solvers by cycling through sequence of solver-specific configurations")
 OPT_INT(plainAddSpecific,                 "plain-add-specific", "",                       0,     0,      1,    "Add specific single options to plain. 0=Nothing. 1=Sweep")
 OPT_BOOL(diversifyNoise,                  "div-noise",  "",                            false,                  "Diversify solvers by adding Gaussian noise on top of numeric parameters")
 OPT_INT(decayDistribution,                "decay-distr",  "",                             1,     1,      2,    "The type of the decay sampling distribution. 1=Gaussian, 2=Uniform     (used for div-noise=1)")
 OPT_INT(decayMean,                        "decay-mean",  "",                             50,     1,    200,    "The mean for sampling the decay value                (used for div-noise=1)")
 OPT_INT(decayStddev,                      "decay-stddev",  "",                           3,      0,   1000,    "The standard deviation for sampling the decay value  (used for div-noise=1)")
 OPT_INT(decayMin,                         "decay-min",  "",                              1,      1,    200,    "The minimum cutoff for sampling a decay value        (used for div-noise=1)")
 OPT_INT(decayMax,                         "decay-max",  "",                              200,    1,    200,    "The maximum cutoff for sampling a decay value        (used for div-noise=1)")
 OPT_INT(diversifyReduce,                  "div-reduce",  "",                             0,      0,      3,    "Toggle to diversify the reduce parameters. 0=Dont, 1=Uniform(with delta optional), 2=Extremes, 3=Gaussian")
 OPT_INT(reduceMin,                        "reduce-min",  "",                             300,    0,    1000,    "The minimium reduce value, in per mille")
 OPT_INT(reduceMax,                        "reduce-max",  "",                             980,    0,    1000,    "The maximum reduce value, in per mille")
 OPT_INT(reduceDelta,                      "reduce-delta", "",                            100,    0,    1000,    "For div-reduce=1: Samples a center reduce value r and give Kissat reducelow=r-delta and reducehigh=r+delta")
 OPT_INT(reduceMean,                       "reduce-mean", "",                             700,    0,    1000,    "For div-reduce=3: The mean reduce value")
 OPT_INT(reduceStddev,                     "reduce-stddev", "",                           150,    0,    1000,    "For div-reduce=3: The stddev of the Gaussian sampled reduce value")
 OPT_BOOL(diversifySeeds,                   "div-seeds", "",                             true,              "Diversify solvers with different random seeds")
 OPT_STRING(satSolverSequence,              "satsolver",  "",                            "C",
 "Sequence of SAT solvers to cycle through (capital letter for true incremental solver, lowercase for pseudo-incremental solving): L|l:Lingeling C|c:CaDiCaL G|g:Glucose k:Kissat m:MergeSAT")

OPTION_GROUP(grpAppSatProof, "app/sat/proof", "Production of UNSAT proofs")
 OPT_STRING(proofDirectory,               "proof-dir", "",                             "",                      "Directory to write partial proofs into (default: -log option")
 OPT_STRING(proofOutputFile,              "proof", "",                                 "",                      "Enable UNSAT proof production, writing final LRAT proof to specified destination (output by rank zero)")
 OPT_BOOL(onTheFlyChecking,               "otfc", "on-the-fly-checking",               false,                   "Enable on-the-fly checking of local derivations; generate and validate signatures for shared clauses")
 OPT_BOOL(onTheFlyCheckModel,             "otfcm", "on-the-fly-check-model",           true,                    "Also check satisfiable assignment in on-the-fly checking (prevents deletion of orig. clauses in one checker per process)")
 OPT_BOOL(distributedProofAssembly,       "dpa", "distributed-proof-assembly",         true,                    "Distributed UNSAT proof assembly into a single file")
 OPT_BOOL(interleaveProofMerging,         "ipm", "interleave-proof-merging",           true,                    "Interleave filtering and merging of proof lines")
 OPT_BOOL(proofDebugging,                 "proof-debugging", "",                       false,                   "Output debugging information into separate files - expensive and large!")
 OPT_INT(compactProof,                    "compact-proof", "",                         0, 0, 2,        "1: Bring clause IDs in a compact shape when writing the final proof, 2: additionally deduplicate clauses")
 OPT_BOOL(uninvertProof,                  "uninvert-proof", "", true, "Uninvert combined inverted proof file")
 OPT_INT(addClauseDeletionStatements,     "cdel", "add-clause-deletions", 2, 0, 2, "0: don't add deletion statements to final proof, 1: add approximately via Bloom filter, 2: add exactly")
 OPT_STRING(extMemDiskDirectory,          "extmem-disk-dir", "",                       ".disk",                 "Directory where to create external memory files") //[[AUTOCOMPLETE_DIRECTORY]]
 OPT_STRING(satPreprocessor,              "sat-preprocessor", "",                      "",                      "Executable which preprocesses CNF file") //[[AUTOCOMPLETE_EXECUTABLE]]
 OPT_FLOAT(satSolvingWallclockLimit,      "sswl", "sat-solving-wallclock-limit",       0,    0, LARGE_INT,      "Cancel job if not done solving after this many seconds (0: no limit)")
 OPT_FLOAT(clauseErrorChancePerMille,     "cecpm", "clause-error-chance-per-mille",    0,    0, 1000,  "Chance per mille for tampering with some literal in a shared clause")
 OPT_FLOAT(derivationErrorChancePerMille, "decpm", "deriv-error-chance-per-mille",     0,    0, 1000,  "Chance per mille for tampering with some on-the-fly checking clause derivation")
