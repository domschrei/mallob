
#pragma once

#include "optionslist.hpp"
#include <climits>

// Application-specific program options for QBF solving.
// memberName                               short option name, long option name          default   min  max

#define QBF_SPLIT_TRIVIAL 0
#define QBF_SPLIT_ITERATIVE_DEEPENING 1

// Example:
OPTION_GROUP(grpAppQbf, "app/qbf", "QBF solving options")
  OPT_INT(qbfSplitStrategy,"qss", "qbf-split-strategy",               0, 0,   1,    
    "Splitting strategy for QBF solving. 0: trivial splitting; 1: bloqqer-based iterative deepening expansion")
  OPT_INT(expansionCostThreshold, "ect", "expansion-cost-threshold", 10'000, 0, INT_MAX,
    "Maximum expansion cost at which bloqqer expands a formula")
  OPT_BOOL(bloqqerLogging, "bloqqer-logging", "", false, "Enable logging into temporary log files for each bloqqer call")
