
#ifndef DOMPASCH_MALLOB_HORDE_CONFIG_HPP
#define DOMPASCH_MALLOB_HORDE_CONFIG_HPP

#include <map>

#include "util/params.hpp"
#include "app/job.hpp"

namespace HordeConfig {
    void applyDefault(Parameters& params, const Job& job);
}

#endif