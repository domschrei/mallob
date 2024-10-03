
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppMaxsat, "app/maxsat", "MaxSAT solving options")

OPT_STRING(maxSatSearchStrategy, "maxsat-search-strat", "", "d", "Sequence of search strategies to employ (d=decreasing i=increasing b=bisection r=naive-refinement)")
OPT_INT(maxSatCardinalityEncoding, "maxsat-card-encoding", "", 3, 0, 3, "0=Warners, 1=DPW, 2=GTE, 3=heuristic choice")
OPT_BOOL(maxSatCombSearch, "maxsat-comb-search", "", false, "Override provided list of search strategies to parallelize over the interval of admissible bounds")
OPT_FLOAT(maxSatCombSkew, "maxsat-comb-skew", "", 0.5, 0, 1, "Skew to cut search intervals with")
OPT_BOOL(maxSatSharedEncoder, "maxsat-shared-encoder", "", false, "Initially add full cardinality constraint encoding to all searches, which renders job descriptions more exchangeable / cacheable")
OPT_FLOAT(maxSatFocusPeriod, "maxsat-focus-period", "", 0, 0, 3600, "Time period (s) until the lowest comb searcher is cancelled (0: never cancel)")
OPT_INT(maxSatFocusMin, "maxsat-focus-min", "", 1, 1, LARGE_INT, "Minimum number of comb searchers to keep alive")
