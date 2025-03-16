
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <string>
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "json_interface.hpp"
#include "util/logger.hpp"
#include "util/static_store.hpp"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/sys/time_period.hpp"
#include "app/sat/job/sat_constants.h"
#include "util/sys/thread_pool.hpp"
#include "app/app_registry.hpp"
#include "data/app_configuration.hpp"
#include "data/job_metadata.hpp"
#include "data/job_result.hpp"
#include "interface/api/job_id_allocator.hpp"
#include "optionslist.hpp"
#include "util/option.hpp"
#include "util/sys/timer.hpp"

JsonInterface::Result JsonInterface::handle(nlohmann::json& inputJson, 
    std::function<void(nlohmann::json&)> feedback) {

    if (!_active || Terminator::isTerminating()) return DISCARD;

    std::string userFile, jobName;
    int id;
    float userPrio, arrival, priority;
    int applicationId;
    bool incremental;
    JobImage* img = nullptr;

    auto baseErrorMsg = "[WARN] Rejecting submission %s - reason: %s\n";

    {
        auto lock = _job_map_mutex.getLock();
        
        // Check and read essential fields from JSON
        if (!inputJson.contains("user") || !inputJson.contains("name")) {
            LOGGER(_logger, V1_WARN, baseErrorMsg, "?.?", "Job file missing essential field(s) \"user\" and/or \"name\".");
            return DISCARD;
        }
        std::string user = inputJson["user"].get<std::string>();
        std::string name = inputJson["name"].get<std::string>();
        jobName = user + "." + name + ".json";
        incremental = inputJson.contains("incremental") ? inputJson["incremental"].get<bool>() : false;

        // Check priority
        priority = inputJson.contains("priority") ? inputJson["priority"].get<float>() : 1.0f;
        if (_params.jitterJobPriorities()) {
            // Jitter job priority
            priority *= 0.99 + 0.01 * Random::rand();
        }
        if (priority <= 0) {
            LOGGER(_logger, V1_WARN, baseErrorMsg, jobName.c_str(), "Priority negative.");
            return DISCARD;
        }

        applicationId = -1;
        if (inputJson.contains("application")) {
            auto appStr = inputJson["application"].get<std::string>();
            applicationId = app_registry::getAppId(appStr);
        }
        if (applicationId == -1) {
            LOGGER(_logger, V1_WARN, "[WARN] No valid application given. Ignoring this file.\n");
            return DISCARD;
        }

        if (inputJson.contains("interrupt") && inputJson["interrupt"].get<bool>()) {
            if (!_job_name_to_id_rev.count(jobName)) {
                LOGGER(_logger, V1_WARN, baseErrorMsg, jobName.c_str(), "Cannot interrupt unknown job.");
                return DISCARD;
            }
            auto [id, rev] = _job_name_to_id_rev.at(jobName);

            // Interrupt a job which is already present
            JobMetadata data;
            data.jobName = jobName;
            data.description = std::unique_ptr<JobDescription>(new JobDescription(id, 0, applicationId));
            data.description->setIncremental(incremental);
            data.description->setRevision(rev);
            data.interrupt = true;
            _job_callback(std::move(data));
            return ACCEPT;
        }

        arrival = inputJson.contains("arrival") ? std::max(Timer::elapsedSeconds(), inputJson["arrival"].get<float>()) 
            : Timer::elapsedSeconds();

        if (incremental && inputJson.contains("precursor")) {

            // This is a new increment of a former job - assign SAME internal ID
            auto precursorName = inputJson["precursor"].get<std::string>() + ".json";
            if (!_job_name_to_id_rev.count(precursorName)) {
                auto warningMsg = "Unknown precursor job \"" + precursorName + "\".";
                LOGGER(_logger, V1_WARN, baseErrorMsg, jobName.c_str(), warningMsg.c_str());
                return DISCARD;
            }
            auto [jobId, rev] = _job_name_to_id_rev[precursorName];
            id = jobId;

            if (inputJson.contains("done") && inputJson["done"].get<bool>()) {

                // Incremental job is notified to be done
                LOGGER(_logger, V3_VERB, "Incremental job #%i is done\n", jobId);
                _job_name_to_id_rev.erase(precursorName);
                for (int rev = 0; rev <= _job_id_to_latest_rev[id]; rev++) {
                    auto key = std::pair<int, int>(id, rev);
                    JobImage* foundImg = _job_id_rev_to_image.at(key);
                    _job_id_rev_to_image.erase(key);
                    delete foundImg;
                }
                _job_id_to_latest_rev.erase(id);

                // Notify client that this incremental job is done
                JobMetadata data;
                data.description = std::unique_ptr<JobDescription>(new JobDescription(id, 0, applicationId));
                data.description->setIncremental(incremental);
                data.jobName = jobName;
                data.done = true;
                _job_callback(std::move(data));
                return ACCEPT_CONCLUDE;

            } else {

                // Job is not done -- add increment to job
                _job_id_to_latest_rev[id] = rev+1;
                _job_name_to_id_rev[jobName] = std::pair<int, int>(id, rev+1);
                img = new JobImage(id, jobName, arrival, feedback);
                img->incremental = true;
                img->baseJson = std::move(inputJson);
                _job_id_rev_to_image[std::pair<int, int>(id, rev+1)] = img;
            }

        } else {

            // Create new internal ID for this job
            if (!_job_name_to_id_rev.count(jobName)) {
                _job_name_to_id_rev[jobName] = std::pair<int, int>(_job_id_allocator.getNext(), 0);
            }
            auto pair = _job_name_to_id_rev[jobName];
            id = pair.first;
            LOGGER(_logger, V3_VERB, "Mapping job \"%s\" to internal ID #%i\n", jobName.c_str(), id);

            // Was job already parsed before?
            if (_job_id_rev_to_image.count(std::pair<int, int>(id, 0))) {
                LOGGER(_logger, V1_WARN, "[WARN] Modification of a file I already parsed! Ignoring.\n");
                throw std::invalid_argument("File was already parsed before");
                return DISCARD;
            }

            img = new JobImage(id, jobName, arrival, feedback);
            img->incremental = incremental;
            img->baseJson = std::move(inputJson);
            _job_id_rev_to_image[std::pair<int, int>(id, 0)] = std::move(img);
            _job_id_to_latest_rev[id] = 0;
        }
    }

    // From here on, use the json inside the JobImage because the parameter JSON has been moved
    assert(img->baseJson != nullptr);
    auto& json = img->baseJson;

    // Initialize new job
    JobDescription* job = new JobDescription(id, priority, applicationId);
    job->setIncremental(incremental);
    job->setRevision(_job_id_to_latest_rev[id]);
    if (json.contains("wallclock-limit")) {
        float limit = TimePeriod(json["wallclock-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setWallclockLimit(limit);
        LOGGER(_logger, V4_VVER, "Job #%i : wallclock time limit %.3f secs\n", id, limit);
    }
    if (json.contains("cpu-limit")) {
        float limit = TimePeriod(json["cpu-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setCpuLimit(limit);
        LOGGER(_logger, V4_VVER, "Job #%i : CPU time limit %.3f CPU secs\n", id, limit);
    }
    if (json.contains("max-demand")) {
        int maxDemand = json["max-demand"].get<int>();
        job->setMaxDemand(maxDemand);
        LOGGER(_logger, V4_VVER, "Job #%i : max demand %i\n", id, maxDemand);
    }
    if (json.contains("assumptions")) {
        job->setPreloadedAssumptions(json["assumptions"].get<std::vector<int>>());
    }
    if (json.contains("internalliterals")) {
        job->setPreloadedLiterals(StaticStore<std::vector<int>>::extract(json["internalliterals"]));
    }
    if (json.contains("literals")) {
        job->setPreloadedLiterals(json["literals"].get<std::vector<int>>());
    }
    if (json.contains("description-id")) {
        const std::string label = json["user"].get<std::string>() + "." + json["description-id"].get<std::string>();
        const int descId = _job_desc_id_allocator.getId(label);
        job->setJobDescriptionId(descId);
        LOGGER(_logger, V4_VVER, "Job #%i rev. %i: set job description ID %i\n", id, job->getRevision(), descId);
    } else job->setJobDescriptionId(0);
    if (json.contains("group-id")) {
        const std::string label = json["user"].get<std::string>() + "." + json["group-id"].get<std::string>();
        const int groupId = _job_desc_id_allocator.getId(label);
        job->setGroupId(groupId);
        LOGGER(_logger, V4_VVER, "Job #%i rev. %i: set group ID %i\n", id, job->getRevision(), groupId);
    }
    job->setArrival(arrival);
    std::vector<std::string> files = json.contains("files") ? 
        json["files"].get<std::vector<std::string>>() : std::vector<std::string>();
    if (json.contains("checksum"))
        job->setChecksum({json["checksum"][0], json["checksum"][1]});

    // Application-specific configuration
    AppConfiguration config;
    config.deserialize(_params.applicationConfiguration());
    if (json.contains("configuration")) {
        auto& jConfig = json["configuration"];
        for (auto it = jConfig.begin(); it != jConfig.end(); ++it) {
            config.map[it.key()] = it.value();
        }
    }
    // legacy support for "content-mode" field at the JSON's top level
    if (!config.map.count("content-mode") && json.contains("content-mode")) {
        config.map["content-mode"] = json["content-mode"].get<std::string>();
    }
    job->setAppConfiguration(std::move(config));
    
    // Translate dependencies (if any) to internal job IDs
    std::vector<int> idDependencies;
    std::vector<std::string> nameDependencies;
    if (json.contains("dependencies")) 
        nameDependencies = json["dependencies"].get<std::vector<std::string>>();
    const std::string ending = ".json";
    for (auto name : nameDependencies) {
        // Convert to the name with ".json" file ending
        name += ending;
        // If the job is not yet known, assign to it a new ID
        // that will be used by the job later
        auto lock = _job_map_mutex.getLock();
        if (!_job_name_to_id_rev.count(name)) {
            _job_name_to_id_rev[name] = std::pair<int, int>(_job_id_allocator.getNext(), 0);
            LOGGER(_logger, V3_VERB, "Forward mapping job \"%s\" to internal ID #%i\n", name.c_str(), _job_name_to_id_rev[name].first);
        }
        idDependencies.push_back(_job_name_to_id_rev[name].first); // TODO inexact: introduce dependencies for job revisions
    }

    // Callback to client: New job arrival.
    JobMetadata metadata;
    metadata.jobName = jobName;
    metadata.description = std::unique_ptr<JobDescription>(job);
    metadata.files = std::move(files);
    metadata.dependencies = std::move(idDependencies);
    _job_callback(std::move(metadata));

    return ACCEPT;
}

void JsonInterface::handleJobDone(JobResult&& result, const JobProcessingStatistics& stats, int applicationId) {

    auto lock = _job_map_mutex.getLock();
    
    JobImage* img = _job_id_rev_to_image[std::pair<int, int>(result.id, result.revision)];
    auto& j = img->baseJson;

    bool useSolutionFile = (_params.pipeSolutions() == MALLOB_PIPE_SOLUTIONS_ALL && result.getSolutionSize() > 0)
        || (_params.pipeSolutions() == MALLOB_PIPE_SOLUTIONS_LARGE && result.getSolutionSize() > 65536);
    auto solutionFile = _output_dir + "/mallob-job-result."
        + std::to_string(result.id) + "." 
        + std::to_string(result.revision) + ".pipe";

    // Pack job result into JSON
    j["internal_id"] = result.id;
    j["internal_revision"] = result.revision;
    j["result"] = { 
        { "resultcode", result.result }, 
        { "resultstring", result.result == RESULT_SAT ? "SAT" : result.result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN" }
    };
    if (useSolutionFile) {
        j["result"]["solution-file"] = solutionFile;
        j["result"]["solution-size"] = result.getSolutionSize();
        mkfifo(solutionFile.c_str(), 0666);
    } else {
        j["result"]["solution"] = app_registry::getJobSolutionFormatter(applicationId)(_params, result, stats);
    }
    j["stats"] = {
        { "time", {
            { "parsing", stats.parseTime },
            { "scheduling", stats.schedulingTime },
            { "first_balancing_latency", stats.latencyOf1stVolumeUpdate },
            { "processing", stats.processingTime },
            { "total", Timer::elapsedSeconds() - img->arrivalTime }
        } },
        { "used_wallclock_seconds" , stats.usedWallclockSeconds },
        { "used_cpu_seconds" , stats.usedCpuSeconds }
    };

    // Send back feedback over whichever connection the job arrived
    img->feedback(j);

    if (useSolutionFile) {
        ProcessWideThreadPool::get().addTask([solutionFile, sol = result.extractSolution()]() {
            
            int fd = open(solutionFile.c_str(), O_WRONLY);
            LOG(V4_VVER, "Writing solution: %i ints (%i,%i,...,%i,%i)\n", sol.size(), 
                sol[0], sol[1], sol[sol.size()-2], sol[sol.size()-1]);
            int numWritten = 0;

            while (numWritten < sol.size()*sizeof(int)) {
                int n = write(fd, ((char*)sol.data())+numWritten, 
                    sol.size() * sizeof(int) - numWritten);
                if (n < 0) break;
                numWritten += n;
            }
            close(fd);
        });
    }

    if (!img->incremental) {
        _job_name_to_id_rev.erase(img->userQualifiedName);
        _job_id_rev_to_image.erase(std::pair<int, int>(result.id, result.revision));
        delete img;
    }
}
