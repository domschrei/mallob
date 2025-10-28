
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_INT(sweepSharingPeriod_ms, "ssp", "sweep-sharing-period", 250, 1, LARGE_INT, "The period (in ms) between initiating sharing operations of Equivalences and Units")
OPT_INT(sweepSolverVerbosity, "", "sweep-solver-verbosity", 0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepResweepChance, "", "sweep-resweep-chance", 1e4, 0, 1e4, "chance that a solver resweeps a variable from a found equivalence (in per mille)")
