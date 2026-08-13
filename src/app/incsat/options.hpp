
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

#define DEFAULT_TRIVIAL_SOLVER_TYPE 1 // CaDiCaL

#if MALLOB_USE_MINISAT
#define MIN_TRIVIAL_SOLVER_TYPE 0
#else
#define MIN_TRIVIAL_SOLVER_TYPE 1
#endif

// Application-specific program options for incremental SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppIncsat, "app/incsat", "Incremental SAT solving options")

OPT_INT(nontrivialSolvingDelayInitial, "nsdi", "nontrivial-solving-delay-initial", 10, 0, LARGE_INT, "Milliseconds to wait with deploying non-trivial solving if an internal stream processor is present (-isp=1)")
OPT_INT(nontrivialSolvingDelaySubsequent, "nsds", "nontrivial-solving-delay-subsequent", 5, 0, LARGE_INT, "Milliseconds to wait with starting previously unsuccessful non-trivial solving if an internal stream processor is present (-isp=1)")
OPT_INT(trivialSolverType, "tst", "trivial-solver-type", DEFAULT_TRIVIAL_SOLVER_TYPE, MIN_TRIVIAL_SOLVER_TYPE, 1, "Type of the trivial sequential solver to run (0=Minisat 1=CaDiCaL)")
OPT_BOOL(internalStreamProcessor, "isp", "", true, "For incremental SAT job streams, run a local single-threaded SAT solver for latency hiding")
