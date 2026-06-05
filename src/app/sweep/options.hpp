
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_BOOL(sweepInitialCongruence,"swic", 	"sweep-initial-congruence", true, "Let each solver do clausal congruence closure once before sweeping")
OPT_BOOL(sweepShuffleWork,		"swsw", 	"sweep-shuffle-work",		true, "Shuffle the work that is provided at the start of each iteration")
OPT_INT(sweepResweepChance, 	"swrc", 	"sweep-resweep-chance", 	0, 0, 1e4, "chance that a solver directly resweeps a variable after it found an equivalence (in per mille)")
OPT_INT(sweepSolverVerbosity,  	"swvrb", 	"sweep-solver-verbosity", 	0, 0, 5, "verbosity of the kissat sweeping solvers in the SWEEP app")
OPT_INT(sweepSolverQuiet,		"swqt", 	"sweep-solver-quiet", 		1, 0, 1, "whether the solver-native messages should be completely disabled (kissat quiet option)")
OPT_INT(sweepMaxIterations, 	"swmi", 	"sweep-max-iterations", 	LARGE_INT, 0, LARGE_INT, "maximum number of iterations")
OPT_INT(sweepMaxDepth, 			"swmd", 	"sweep-max-depth", 			LARGE_INT, 2, LARGE_INT, "maximum environment depth")
OPT_BOOL(sweepXTCSsend,			"swxs", 	"sweep-xj-send",			true, "send units and equivalences via cross-job-communication -cjc")
OPT_BOOL(sweepXTCSrecv,			"swxr", 	"sweep-xj-recv",			true, "receive units via cross-job-communication -cjc")
OPT_FLOAT(sweepSharingPeriod,	"swsp", 	"sweep-sharing-period", 	0.050, 0.001, LARGE_INT, "The sharing period (of equivalences and units), in seconds")
OPT_INT(sweepMaxKittenProp,		"swmp", 	"sweep-max-kitten-prop",	1000000,1000,LARGE_INT, "Maximum number of kitten propagations per kitten SAT Call")
OPT_BOOL(sweepSignalKitten,		"swsk", 	"sweep-signal-kitten",		true, "Signal to kittens when an iteration has been skipped such that they can exit their propagation loop immediately")
OPT_FLOAT(sweepSkipRatio,		"swskr",    "sweep-skip-ratio",			0.01, 0, 1, "minimum ratio of found Eqs+Units versus number of swept variables to not skip an iteration")
OPT_FLOAT(sweepSkipWindowSecs,	"swskw",	"sweep-skip-window",		2.0, 0, LARGE_INT, "size of moving window for which the skip-ratio is checked (in seconds)")
OPT_INT(sweepMaxBadIterations,	"swmbi",	"sweep-max-bad-iters",		3, 0, LARGE_INT, "max iterations allowed to end without being successful before the Job is terminated")
OPT_BOOL(sweepShowLagWarn,		"swslw", 	"sweep-show-lag-warn",		false, "show 'lagging' solvers that are in the same sweep(...) call since a long time")

