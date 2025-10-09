
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_INT(sweepSharingPeriod_ms, "ssp", "sweep-sharing-period", 250, 10, LARGE_INT, "The period (in ms) between initiating Equivalence+Units sharing operations")
OPT_INT(sweepSolverVerbosity, "", "sweep-solver-verbosity", 0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
