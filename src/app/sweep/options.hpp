
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_INT(sweepSharingPeriod_ms,	"swpsp", 	"sweep-sharing-period", 	250, 1, LARGE_INT, "The period (in ms) between initiating sharing operations of Equivalences and Units")
OPT_INT(sweepSolverVerbosity,  	"swpvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swpqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepResweepChance, 	"swprc", 	"sweep-resweep-chance", 	1e4, 0, 1e4, "chance that a solver resweeps a variable from a found equivalence (in per mille)")
OPT_INT(sweepRounds, 			"swprds", 	"sweep-rounds", 			1, 0, LARGE_INT, "number of full sweep rounds over all variables")
OPT_BOOL(sweepCongruence,  		"swpcg", 	"sweep-congruence", 		false, "One solver at the root node does clausal congruence closure instead of sweeping")
