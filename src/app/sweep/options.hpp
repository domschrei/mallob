
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_BOOL(sweepInitialCongruence,"swpic", 	"sweep-initial-congruence", true, "Let all solvers do one iteration of congruence before sweeping")
OPT_BOOL(sweepShuffleWork,		"swpsw", 	"sweep-shuffle-work",		true, "Shuffle the work that is provided at the start of each iteration")
OPT_INT(sweepResweepChance, 	"swprc", 	"sweep-resweep-chance", 	0, 0, 1e4, "chance that a solver directly resweeps a variable after a found equivalence (in per mille)")
OPT_INT(sweepSolverVerbosity,  	"swpvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swpqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepMaxIterations, 	"swpmi", 	"sweep-max-iterations", 	99, 0, LARGE_INT, "max number of completed sweep iterations over all variables")
OPT_INT(sweepMaxDepth, 			"swpmd", 	"sweep-max-depth", 			99, 2, LARGE_INT, "maximum environment depth for sweeping")
OPT_BOOL(sweepXTCSsend,			"swpxs", 	"sweep-xj-send",			true, "send units and equivalences via cross-job-communication -cjc")
OPT_BOOL(sweepXTCSrecv,			"swpxr", 	"sweep-xj-recv",			true, "receive units via cross-job-communication -cjc")
OPT_FLOAT(sweepSharingPeriod,	"swpsp", 	"sweep-sharing-period", 	0.050, 0.001, LARGE_INT, "The sharing period (of equivalences and units), in seconds")
OPT_INT(sweepMaxKittenProp,		"swpmkp", 	"sweep-max-kitten-prop",	1000000,1000,LARGE_INT, "Maximum number of kitten propagations per kitten SAT Call")
OPT_BOOL(sweepSignalKitten,		"swpsk", 	"sweep-signal-kitten",		true, "Signal to kitten solvers when an iteration has been skipped, such that they exit immediately")
OPT_FLOAT(sweepSkipRatio,		"swpskr",   "sweep-skip-ratio",			0.01, 0, 1, "minimum ratio of found Eqs+Units versus number of swept variables to not skip an iteration")
OPT_INT(sweepSkipWindow,		"swpskw",	"sweep-skip-window",		100, 1, LARGE_INT, "number of last sharing rounds for which the skip-ratio is checked")
OPT_INT(sweepSkipCount,			"swpskc",	"sweep-skip-count",			3, 1, LARGE_INT, "number of allowed skips after which the job is terminated")
