
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppSmt, "app/smt", "SMT solving options")

OPT_STRING(smtOutputFile, "smt-out-file", "", "", "Path to file to write SMT output and solutions to")
OPT_STRING(bitwuzlaArgs, "smt-args", "", "", "Additional comma-separated arguments (no whitespaces) to forward to Bitwuzla")
