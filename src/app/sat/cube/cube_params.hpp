#ifndef MSCHICK_CUBE_CONFIG_HPP
#define MSCHICK_CUBE_CONFIG_HPP

#include <map>

#include "app/job.hpp"
#include "util/params.hpp"

namespace CubeConfig {
void applyDefault(Parameters& params, const Job& job) {
    params["apprank"] = std::to_string(job.getIndex());             // rank within application
    params["mpisize"] = std::to_string(job.getGlobalNumWorkers());  // size of worker comm
    std::string identifier = std::string(job.toStr());
    params["jobstr"] = identifier;

    params["mpirank"] = std::to_string(job.getMyMpiRank());  // rank of this node
    params["jobid"] = std::to_string(job.getId());
    params["starttime"] = std::to_string(Timer::getStartTime());
};
}  // namespace CubeConfig

#endif