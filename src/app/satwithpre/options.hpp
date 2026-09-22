
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

#define SATWITHPRE_OPT_OVERRIDES "-satsolver=k_k_l+[k_]{13} -ilbd=0 -rlbd=3"
// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSatwithpre, "app/satwithpre", "SAT solving with preprocessing options")

OPT_INT(preprocessBalancing, "pb", "preprocess-balancing", 1, -1, 2, "How to balance original vs. preprocessed task: -1=never start original task, 0=drop original immediately, 1=replace original gradually, 2=run both indefinitely")
OPT_FLOAT(preprocessJobPriority, "pjp", "preprocess-job-priority", LARGE_INT, 0.0001f, LARGE_INT, "Job priority to assign to preprocessed task")
OPT_BOOL(preprocessSweepnSat, "preprocess-sweepnsat", "", false, "run concurrently a SWEEP-App and a SAT-App, with cross job unit- and equivalence-sharing (Sweep'n'SAT from FMCAD26)")
OPT_FLOAT(preprocessSweepPriority, "psp", "preprocess-sweep-priority", 1.000f, 0.0001f, LARGE_INT, "Ressource split ratio between SWEEP and SAT Job in Sweep'n'Sat. Default 1.000f is a 50/50 ressource split")
OPT_FLOAT(preprocessExpansionFactor, "pef", "preprocess-expansion-factor", 1.f, 0.0001f, LARGE_INT, "Expand preprocessed task over -pef times the task's running time up to that point")
OPT_STRING(overrideSatOptions, "oso", "override-sat-options", "",
    "In each distributed SAT sub-task, override the SAT solver process configuration with these Mallob options")
OPT_STRING(preprocessConfig, "preprocess-config", "", "config/satwithpre/actors_default.json",
    "JSON file to configure the cascading preprocessing actors")
OPT_STRING(preprocessLogDir, "preprocess-log", "", "", "Directory to log preprocessor formulas and models to")
