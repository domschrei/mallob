
#pragma once

#include <functional>
#include <atomic>

#include "util/logger.hpp"
#include "util/hashing.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_metadata.hpp"
#include "util/json.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"

class Parameters; // fwd declaration

class JsonInterface {

public:
    enum Status {NEW, PENDING, DONE, INTRODUCED};
    struct JobImage {
        int id;
        std::string userQualifiedName;
        float arrivalTime;
        bool incremental = false;
        nlohmann::json baseJson;
        std::function<void(nlohmann::json&)> feedback;

        JobImage() = default;
        JobImage(int id, const std::string& userQualifiedName, float arrivalTime, 
                std::function<void(nlohmann::json&)> feedback) 
            : id(id), userQualifiedName(userQualifiedName), arrivalTime(arrivalTime), 
                feedback(feedback) {}
    };

private:
    const Parameters& _params;
    Logger _logger;

    Mutex _job_map_mutex;
    std::atomic_int _running_id;

    std::function<void(JobMetadata&&)> _job_callback;
    
    robin_hood::unordered_node_map<std::string, std::pair<int, int>> _job_name_to_id_rev;
    robin_hood::unordered_node_map<int, int> _job_id_to_latest_rev;
    robin_hood::unordered_node_map<std::pair<int, int>, JobImage*, IntPairHasher> _job_id_rev_to_image;

public:
    JsonInterface(int clientRank, const Parameters& params, Logger&& logger, 
            std::function<void(JobMetadata&&)> jobCallback) : 
        _params(params),
        _logger(std::move(logger)),
        _job_map_mutex(),
        _running_id(clientRank * 100000 + 1),
        _job_callback(jobCallback) {}
    ~JsonInterface() {}

    // User-side events
    enum Result {ACCEPT, ACCEPT_CONCLUDE, DISCARD};
    Result handle(nlohmann::json& json, std::function<void(nlohmann::json&)> feedback);

    // Mallob-side events
    void handleJobDone(JobResult&& result, const JobDescription::Statistics& stats, int applicationId);
};
