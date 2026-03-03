
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_FLOAT(sweepSharingPeriod,	"swpsp", 	"sweep-sharing-period", 	0.250, 0.001, LARGE_INT, "The period (in seconds) between initiating sharing operations of Equivalences and Units")
OPT_INT(sweepSolverVerbosity,  	"swpvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swpqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepResweepChance, 	"swprc", 	"sweep-resweep-chance", 	1e4, 0, 1e4, "chance that a solver resweeps a variable from a found equivalence (in per mille)")
OPT_INT(sweepIterations, 		"swpite", 	"sweep-iterations", 			1, 0, LARGE_INT, "number of full sweep iterations over all variables")
OPT_BOOL(sweepCongruence,  		"swpcg", 	"sweep-congruence", 		false, "One solver at the root node does clausal congruence closure instead of sweeping")
OPT_BOOL(sweepDeduplicate,		"swpdd", 	"sweep-deduplicate",		true,  "Deduplicate units and equivalences during sharing aggregation")
OPT_INT(sweepMaxGrowthIteration, "swmgi", 	"sweep-max-growth-iteration", 	2, 0, LARGE_INT, "the last iteration where the kissat sweeping environment size is increased (#clauses, #vars, depth) ")
OPT_INT(sweepMaxEmptyRounds,    "swpmer",   "sweep-max-empty-rounds",    3, 1, LARGE_INT, "the maximum number of tolerated empty rounds (not a single unit or eqs found) before sweeping is terminated early")

