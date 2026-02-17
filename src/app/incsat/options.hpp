
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for incremental SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppIncsat, "app/incsat", "Incremental SAT solving options")

OPT_INT(nontrivialSolvingDelay, "nsd", "nontrivial-solving-delay", 25, 0, LARGE_INT, "Milliseconds to wait with non-trivial solving if an internal stream processor is present (-isp=1)")
