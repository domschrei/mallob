
#include "horde_config.hpp"

#include "util/sys/timer.hpp"

void HordeConfig::applyDefault(Parameters& params, const Job& job) {

    if (params.getIntParam("md") <= 1 && params.getIntParam("t") <= 1) {
        // One thread on one node: do not diversify anything, but keep default solver settings
        params["diversify"] = "0"; // no diversification
    } else if (params.isSet("nophase")) {
        // Do not do sparse random ("phase") diversification
        params["diversify"] = "4"; // native diversification only
    } else {
        params["diversify"] = "7"; // sparse random + native diversification
    }
    params["fd"]; // filter duplicate clauses
    params["i"] = "0"; // #microseconds to sleep during solve loop
    params["apprank"] = std::to_string(job.getIndex()); // rank within application
    params["mpisize"] = std::to_string(job.getGlobalNumWorkers()); // size of worker comm
    std::string identifier = std::string(job.toStr());
    params["jobstr"] = identifier;
    
    params["mpirank"] = std::to_string(job.getMyMpiRank()); // rank of this node
    params["jobid"] = std::to_string(job.getId());
    params["starttime"] = std::to_string(Timer::getStartTime());
}