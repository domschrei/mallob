
#pragma once

#include "optionslist.hpp"

// Add your application-specific options here

OPTION_GROUP(grpAppSweep, "app/sweep", "Options for application sweep")
OPT_INT(sweepThreads, "sweep-threads", "", 4, 1, 12, "Number of threads that are created for sweeping")
