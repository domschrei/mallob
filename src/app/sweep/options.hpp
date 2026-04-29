
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_FLOAT(sweepSharingPeriod,	"swpsp", 	"sweep-sharing-period", 	0.250, 0.001, LARGE_INT, "The period (in seconds) between initiating sharing operations of Equivalences and Units")
OPT_INT(sweepSolverVerbosity,  	"swpvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swpqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepResweepChance, 	"swprc", 	"sweep-resweep-chance", 	1e4, 0, 1e4, "chance that a solver resweeps a variable from a found equivalence (in per mille)")
OPT_INT(sweepMaxIterations, 	"swpmi", 	"sweep-max-iterations", 	3, 0, LARGE_INT, "max number of completed sweeps over all variables")
OPT_BOOL(sweepInitialCongruence,"swpic", 	"sweep-initial-congruence", true, "Let all solvers do one iteration of congruence before sweeping")
OPT_INT(sweepMinDepth,			"swpmind",  "sweep-min-depth",			2, 2, LARGE_INT, "minimum environment depth for sweeping")
OPT_INT(sweepMaxDepth, 			"swpmaxd", 	"sweep-max-depth", 			4, 2, LARGE_INT, "maximum environment depth for sweeping")
OPT_INT(sweepMaxEmptyRounds,    "swpmer",   "sweep-max-empty-rounds",   5, 1, LARGE_INT, "the maximum number of tolerated empty rounds (not a single unit or eqs found) before sweeping is terminated early")
OPT_INT(sweepMinExitSwept,      "swpmes",   "sweep-min-exit-swept",     40000, 0, LARGE_INT, "minimum number of sweeps after which earlyexit gets checked. 0 = never exit early")
OPT_INT(sweepTermNoProgress,    "swptnp",   "sweep-term-no-progress",   1, 0, 1, "terminate sweeping after an iteration made no progress at all")
OPT_BOOL(sweepShuffleWork,		"swpsw", 	"sweep-shuffle-work",		true, "Shuffle the work that is provided at the start of each iteration")
OPT_BOOL(sweepToCompletion,		"swptoc", 	"sweep-to-completion",		false, "Sweep indefinitely to completion (or timeout). Increase env size only when no more progress")
OPT_BOOL(sweepXJsendTo,			"swpxs", 	"sweep-xj-send",			true, "Sweep sends its units and equivalences via cjc to the concurrent SAT job ")
OPT_BOOL(sweepXJrecvFrom,		"swpxr", 	"sweep-xj-recv",			true, "Sweep imports units via cjc from the concurrent SAT job")
