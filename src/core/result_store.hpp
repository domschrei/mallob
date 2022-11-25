
#pragma once

#include "util/hashing.hpp"
#include "util/robin_hood.hpp"
#include "data/job_result.hpp"

class ResultStore {

private:
    robin_hood::unordered_map<std::pair<int, int>, JobResult, IntPairHasher> _pending_results;

public:
    void store(int jobId, int revision, JobResult&& result) {
        auto key = std::pair<int, int>(jobId, revision);
        _pending_results[key] = std::move(result);
    }

    std::vector<uint8_t> retrieveSerialization(int jobId, int revision) {
        auto key = std::pair<int, int>(jobId, revision);
        assert(_pending_results.count(key));
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
