
#pragma once

#include "util/hashing.hpp"
#include "util/logger.hpp"
#include "util/robin_hood.hpp"
#include "data/job_result.hpp"

class ResultStore {

private:
    robin_hood::unordered_map<std::pair<int, int>, JobResult, IntPairHasher> _pending_results;

public:
    void store(int jobId, int revision, JobResult&& result) {
        auto key = std::pair<int, int>(jobId, revision);
        _pending_results[key] = std::move(result);
        assert(_pending_results[key].hasSerialization());
    }

    std::vector<uint8_t> retrieveSerialization(int jobId, int revision) {
        auto key = std::pair<int, int>(jobId, revision);
        if (!_pending_results.count(key)) {
            LOG(V1_WARN, "[WARN] No job result for #%i rev. %i present – returning UNKNOWN\n", jobId, revision);
            JobResult res;
            res.id = jobId;
            res.revision = revision;
            res.result = 0;
            return res.serialize();
        }
        JobResult& result = _pending_results.at(key);
        assert(result.id == jobId);
        result.updateSerialization();
        auto serialization = result.moveSerialization();
        _pending_results.erase(key);
        return serialization;
    }
    
    void discard(int jobId, int revision) {
        _pending_results.erase(std::pair<int, int>(jobId, revision));
    }
};
