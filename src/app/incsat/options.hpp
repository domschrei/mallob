
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

#if MALLOB_USE_MINISAT
#define DEFAULT_TRIVIAL_SOLVER_TYPE 0
#else
#define DEFAULT_TRIVIAL_SOLVER_TYPE 1
#endif

// Application-specific program options for incremental SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppIncsat, "app/incsat", "Incremental SAT solving options")

OPT_INT(nontrivialSolvingDelay, "nsd", "nontrivial-solving-delay", 25, 0, LARGE_INT, "Milliseconds to wait with non-trivial solving if an internal stream processor is present (-isp=1)")
OPT_INT(trivialSolverType, "tst", "trivial-solver-type", DEFAULT_TRIVIAL_SOLVER_TYPE, 0, 1, "Type of the trivial sequential solver to run (0=Minisat 1=CaDiCaL)")
