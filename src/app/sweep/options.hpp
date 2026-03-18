
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_FLOAT(sweepSharingPeriod,	"swpsp", 	"sweep-sharing-period", 	0.250, 0.001, LARGE_INT, "The period (in seconds) between initiating sharing operations of Equivalences and Units")
OPT_INT(sweepSolverVerbosity,  	"swpvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swpqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepResweepChance, 	"swprc", 	"sweep-resweep-chance", 	1e4, 0, 1e4, "chance that a solver resweeps a variable from a found equivalence (in per mille)")
OPT_INT(sweepMaxIterations, 	"swpmi", 	"sweep-max-iterations", 	3, 0, LARGE_INT, "max number of completed sweeps over all variables")
OPT_BOOL(sweepCongruence,  		"swpcg", 	"sweep-congruence", 		false, "One solver at the root node does clausal congruence closure instead of sweeping")
OPT_BOOL(sweepDeduplicate,		"swpdd", 	"sweep-deduplicate",		true,  "Deduplicate units and equivalences during sharing aggregation")
OPT_INT(sweepMaxDepth, 			"swpmd", 	"sweep-max-depth", 			3, 1, LARGE_INT, "the maximum environment depth for sweeping")
OPT_INT(sweepMaxEmptyRounds,    "swpmer",   "sweep-max-empty-rounds",    5, 1, LARGE_INT, "the maximum number of tolerated empty rounds (not a single unit or eqs found) before sweeping is terminated early")
OPT_BOOL(sweepIndividualSweepIters,		"swpisi", 	"sweep-individual-sweep-iters",		true,  "Kissat start a new sweeper for each sweeping iteration. Closer to sequential model, and allows easier substitute integration between iterations")

