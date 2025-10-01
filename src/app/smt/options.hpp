
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSmt, "app/smt", "SMT solving options")

OPT_STRING(smtSolutionFile, "smt-sol-file", "", "", "Path to file to write SMT level solutions to")
