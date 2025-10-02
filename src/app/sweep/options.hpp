
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_INT(sweepSharingPeriod_ms, "sweep-sharing-period", "ssp", 250, 10, LARGE_INT, "The period (in ms) between initiating Equivalence+Units sharing operations")
