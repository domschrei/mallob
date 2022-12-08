
#pragma once

#include "optionslist.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSat, "app/sat", "SAT solving options")
 OPT_BOOL(abortNonincrementalSubprocess,    "ans", "abort-noninc-subproc",               false,                   
    "Abort (hence restart) each sub-process which works (partially) non-incrementally upon the arrival of a new revision")
 OPT_INT(maxLiteralsPerThread,              "mlpt", "max-lits-per-thread",               50000000, 0,   MAX_INT,    
    "If formula is larger than threshold, reduce #threads per PE until #threads=1 or until limit is met \"on average\"")
 OPT_STRING(satEngineConfig,                "sec", "sat-engine-config",                  "",                      
    "Supply config for SAT engine subprocess [internal option, do not use]")

OPTION_GROUP(grpAppSatSharing, "app/sat/sharing", "Clause sharing configuration")
 OPT_INT(bufferedImportedClsGenerations,    "bicg", "buffered-imported-cls-generations", 4,        1,   LARGE_INT, 
    "Number of subsequent full clause sharings to fit in each solver's import buffer")
 OPT_INT(clauseBufferBaseSize,              "cbbs", "clause-buffer-base-size",           1500,     0,   MAX_INT,   
    "Clause buffer base size in integers")
 OPT_FLOAT(clauseBufferDiscountFactor,      "cbdf", "clause-buffer-discount",            0.9,      0.5, 1,            
    "Clause buffer discount factor: reduce buffer size per PE by <factor> each depth")
 OPT_FLOAT(clauseFilterClearInterval,       "cfci", "clause-filter-clear-interval",      20,       -1,  LARGE_INT,     
    "Set clear interval of clauses in solver filters (-1: never clear, 0: always clear")
 OPT_BOOL(groupClausesByLengthLbdSum,       "gclls", "group-by-length-lbd-sum",          false,                   
    "Group and prioritize clauses in buffers by the sum of clause length and LBD score")
 OPT_INT(maxLbdPartitioningSize,            "mlbdps", "max-lbd-partition-size",          8,        1,   LARGE_INT,      
    "Store clauses with up to this LBD in separate buckets")
 OPT_INT(minNumChunksForImportPerSolver,    "mcips", "min-import-chunks-per-solver",     10,       1,   LARGE_INT,      
    "Min. number of cbbs-sized chunks for buffering incoming clauses for import per solver")
 OPT_INT(numChunksForExport,                "nce", "export-chunks",                      20,       1,   LARGE_INT,      
    "Number of cbbs-sized chunks for buffering produced clauses for export")
 OPT_INT(qualityClauseLengthLimit,          "qcll", "quality-clause-length-limit",       8,        0,   LARGE_INT,      
    "Clauses up to this length are considered \"high quality\"")
 OPT_INT(qualityLbdLimit,                   "qlbdl", "quality-lbd-limit",                2,        0,   LARGE_INT,      
    "Clauses with an LBD score up to this value are considered \"high quality\"")
 OPT_BOOL(reshareImprovedLbd,               "ril", "reshare-improved-lbd",               false,                   
    "Reshare clauses (regardless of their last sharing epoch) if their LBD improved")
 OPT_INT(strictClauseLengthLimit,           "scll", "strict-clause-length-limit",        30,       0,   LARGE_INT,      
    "Only clauses up to this length will be shared")
 OPT_INT(strictLbdLimit,                    "slbdl", "strict-lbd-limit",                 30,       0,   LARGE_INT,      
    "Only clauses with an LBD score up to this value will be shared")

OPTION_GROUP(grpAppSatDiversification, "app/sat/diversification", "Diversification options")
 OPT_FLOAT(inputShuffleProbability,         "isp", "input-shuffle-probability",          1,        0,   1,
    "Probability for a solver (never the 1st one of a kind) to shuffle the order of clauses in the input to some degree")
 OPT_BOOL(phaseDiversification,             "phasediv", "",                              true,
    "Diversify solvers based on phase in addition to native diversification")
 OPT_STRING(satSolverSequence,              "satsolver",  "",                            "C",
    "Sequence of SAT solvers to cycle through (capital letter for true incremental solver, lowercase for pseudo-incremental solving): L|l:Lingeling C|c:CaDiCaL G|g:Glucose k:Kissat m:MergeSAT")

OPTION_GROUP(grpAppSatHistory, "app/sat/history", "Clause history configuration [experimental]")
 OPT_INT(clauseHistoryAggregationFactor,  "chaf", "clause-history-aggregation",          5,        1,   LARGE_INT, 
    "Aggregate historic clause batches by this factor")
 OPT_INT(clauseHistoryShortTermMemSize,   "chstms", "clause-history-shortterm-size",     10,       1,   LARGE_INT, 
    "Save this many \"full\" aggregated epochs until reducing them")
 OPT_BOOL(collectClauseHistory,           "ch", "collect-clause-history",                false,                   
    "Employ clause history collection mechanism")
