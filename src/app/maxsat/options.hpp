
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppMaxsat, "app/maxsat", "MaxSAT solving options")