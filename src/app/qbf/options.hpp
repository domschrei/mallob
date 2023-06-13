
#pragma once

#include "optionslist.hpp"

// Application-specific program options for QBF solving.
// memberName                               short option name, long option name          default   min  max

// Example:
// OPTION_GROUP(grpAppSat, "app/sat", "SAT solving options")
//  OPT_BOOL(abortNonincrementalSubprocess,    "ans", "abort-noninc-subproc",               false,                   
//     "Abort (hence restart) each sub-process which works (partially) non-incrementally upon the arrival of a new revision")
//  OPT_INT(maxLiteralsPerThread,              "mlpt", "max-lits-per-thread",               50000000, 0,   MAX_INT,    
//     "If formula is larger than threshold, reduce #threads per PE until #threads=1 or until limit is met \"on average\"")
//  OPT_STRING(satEngineConfig,                "sec", "sat-engine-config",                  "",                      
//     "Supply config for SAT engine subprocess [internal option, do not use]")
//  OPT_BOOL(copyFormulaeFromSharedMem,        "cpshm", "",                                           false,
//     "Copy each formula + assumptions from shared memory to local memory before launching solvers")
