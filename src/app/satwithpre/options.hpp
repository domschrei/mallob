
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSatwithpre, "app/satwithpre", "SAT solving with preprocessing options")

OPT_INT(preprocessBalancing, "pb", "preprocess-balancing", 1, -1, 2, "How to balance original vs. preprocessed task: -1=never start original task, 0=drop original immediately, 1=replace original gradually, 2=run both indefinitely")
OPT_FLOAT(preprocessJobPriority, "pjp", "preprocess-job-priority", LARGE_INT, 0.0001f, LARGE_INT, "Job priority to assign to preprocessed task")
OPT_FLOAT(preprocessExpansionFactor, "pef", "preprocess-expansion-factor", 1.f, 0.0001f, LARGE_INT, "Expand preprocessed task over -pef times the task's running time up to that point")
OPT_BOOL(preprocessLingeling, "pl", "preprocess-lingeling", false, "Additionally run Lingeling as a preprocessor")
OPT_BOOL(terminateAbruptly, "terminate-abruptly", "", false, "Upon termination, avoid waiting for preprocessors to finish")
