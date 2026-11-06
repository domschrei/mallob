#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppMus, "app/mus", "MUS solving options")

OPT_INT(musOption, "musOption", "", 1, 0, 1, "test mus option")
OPT_BOOL(musNonsharingEngine,  "musNonsharingEngine", "", false, "Ignore sharing and solve the MUS problem just within one engine")
OPT_BOOL(musRotation,  "musRotation", "", false, "Do model rotation on satisfiable MUS SAT calls")
