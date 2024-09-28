
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppMaxsat, "app/maxsat", "MaxSAT solving options")

OPT_STRING(maxSatSearchStrategy, "maxsat-search-strat", "", "d", "Sequence of search strategies to employ (d=decreasing i=increasing b=bisection r=naive-refinement)")
OPT_INT(maxSatCardinalityEncoding, "maxsat-card-encoding", "", 3, 0, 3, "0=Warners, 1=DPW, 2=GTE, 3=heuristic choice")
OPT_BOOL(maxSatCombSearch, "maxsat-comb-search", "", false, "Override provided list of search strategies to parallelize over the interval of admissible bounds")
OPT_FLOAT(maxSatCombMinRatio, "maxsat-comb-min-ratio", "", 0, 0, 1, "Minimum ratio between lower bound and upper bound to start comb search at")
OPT_FLOAT(maxSatCombMaxRatio, "maxsat-comb-max-ratio", "", 1, 0, 1, "Maximum ratio between lower bound and upper bound to stop comb search at")
OPT_BOOL(maxSatSharedEncoder, "maxsat-shared-encoder", "", false, "Initially add full cardinality constraint encoding to all searches, which renders job descriptions more exchangeable / cacheable")
