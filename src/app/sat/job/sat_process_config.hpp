
#pragma once

#include <stdlib.h>
#include <sys/types.h>
#include <sstream>
#include <string>

class Parameters;

struct SatProcessConfig {

    long starttimeSecs;
    long starttimeNsecs;
    int apprank;
    int mpirank;
    int mpisize;

    int jobid;
    bool incremental;
    int firstrev;
    int threads;
    int maxBroadcastedLitsPerCycle;
    int recoveryIndex;
    int nbPreviousBalancingEpochs;

    SatProcessConfig() {}
    SatProcessConfig(const std::string& packed) {
        std::stringstream s_stream(packed);
        std::string substr;
        getline(s_stream, substr, ','); starttimeSecs = atol(substr.c_str());
        getline(s_stream, substr, ','); starttimeNsecs = atol(substr.c_str());
        getline(s_stream, substr, ','); apprank = atoi(substr.c_str());
        getline(s_stream, substr, ','); mpirank = atoi(substr.c_str());
        getline(s_stream, substr, ','); mpisize = atoi(substr.c_str());
        getline(s_stream, substr, ','); jobid = atoi(substr.c_str());
        getline(s_stream, substr, ','); incremental = substr == "1";
        getline(s_stream, substr, ','); firstrev = atoi(substr.c_str());
        getline(s_stream, substr, ','); threads = atoi(substr.c_str());
        getline(s_stream, substr, ','); maxBroadcastedLitsPerCycle = atoi(substr.c_str());
        getline(s_stream, substr, ','); recoveryIndex = atoi(substr.c_str());
        getline(s_stream, substr, ','); nbPreviousBalancingEpochs = atoi(substr.c_str());
    }

    std::string getSharedMemId(pid_t pid) const {
        return  "/edu.kit.iti.mallob." + std::to_string(pid) + "." 
            + std::to_string(mpirank) + ".#" + std::to_string(jobid) 
            + (recoveryIndex == 0 ? std::string() : "~" + std::to_string(recoveryIndex));
    }

    std::string toString() const {
        std::string out = "";
        out += std::to_string(starttimeSecs) + ",";
        out += std::to_string(starttimeNsecs) + ",";
        out += std::to_string(apprank) + ",";
        out += std::to_string(mpirank) + ",";
        out += std::to_string(mpisize) + ",";
        out += std::to_string(jobid) + ",";
        out += std::to_string(incremental?1:0) + ",";
        out += std::to_string(firstrev) + ",";
        out += std::to_string(threads) + ",";
        out += std::to_string(maxBroadcastedLitsPerCycle) + ",";
        out += std::to_string(recoveryIndex) + ",";
        out += std::to_string(nbPreviousBalancingEpochs);
        return out;
    }

    std::string getJobStr() const {
        return "#" + std::to_string(jobid);
    }
};
