
#include <iomanip>

#include "job_file_adapter.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/time_period.hpp"
#include "util/random.hpp"
#include "app/sat/sat_constants.h"
#include "util/sys/terminator.hpp"

void JobFileAdapter::handleNewJob(const FileWatcher::Event& event, Logger& log) {

    if (Terminator::isTerminating()) return;

    log.log(V3_VERB, "New job file event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    nlohmann::json j;
    std::string userFile, jobName;
    int id;
    float userPrio, arrival;

    {
        auto lock = _job_map_mutex.getLock();
        
        // Attempt to read job file
        std::string eventFile = getJobFilePath(event, NEW);
        if (!FileUtils::isRegularFile(eventFile)) {
            return; // File does not exist (any more)
        }
        try {
            std::ifstream i(eventFile);
            i >> j;
        } catch (const nlohmann::detail::parse_error& e) {
            log.log(V1_WARN, "Parse error on %s: %s\n", eventFile.c_str(), e.what());
            return;
        }

        // Check and read essential fields from JSON
        if (!j.contains("user") || !j.contains("name") || !j.contains("file")) {
            log.log(V1_WARN, "Job file missing essential field(s). Ignoring this file.\n");
            return;
        }
        std::string user = j["user"].get<std::string>();
        std::string name = j["name"].get<std::string>();
        jobName = user + "." + name + ".json";

        // Get user definition
        userFile = getUserFilePath(user);
        nlohmann::json jUser;
        try {
            std::ifstream i(userFile);
            i >> jUser;
        } catch (const nlohmann::detail::parse_error& e) {
            log.log(V1_WARN, "Unknown user or invalid user definition: %s\n", e.what());
            return;
        }
        if (!jUser.contains("id") || !jUser.contains("priority")) {
            log.log(V1_WARN, "User file %s missing essential field(s). Ignoring job file with this user.\n", userFile.c_str());
            return;
        }
        if (jUser["id"].get<std::string>() != user) {
            log.log(V1_WARN, "User file %s has inconsistent user ID. Ignoring job file with this user.\n", userFile.c_str());
            return;
        }
        
        // Get internal ID for this job
        if (!_job_name_to_id.count(jobName)) _job_name_to_id[jobName] = _running_id++;
        id = _job_name_to_id[jobName];
        log.log(V3_VERB, "Mapping job \"%s\" to internal ID #%i\n", jobName.c_str(), id);

        // Was job already parsed before?
        if (_job_id_to_image.count(id)) {
            log.log(V1_WARN, "Modification of a file I already parsed! Ignoring.\n");
            return;
        }
        
        userPrio = jUser["priority"].get<float>();
        arrival = j.contains("arrival") ? j["arrival"].get<float>() : Timer::elapsedSeconds();
        _job_id_to_image[id] = JobImage(id, jobName, event.name, arrival);

        // Remove original file, move to "pending"
        FileUtils::rm(eventFile);
        std::ofstream o(getJobFilePath(id, PENDING));
        o << std::setw(4) << j << std::endl;
    }

    // Initialize new job
    float priority = userPrio * (j.contains("priority") ? j["priority"].get<float>() : 1.0f);
    if (_params.isNotNull("jjp")) {
        // Jitter job priority
        priority *= 0.99 + 0.01 * Random::rand();
    }
    JobDescription* job = new JobDescription(id, priority, /*incremental=*/false);
    if (j.contains("wallclock-limit")) {
        float limit = TimePeriod(j["wallclock-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setWallclockLimit(limit);
        log.log(V4_VVER, "Job #%i : wallclock time limit %.3f secs\n", id, limit);
    }
    if (j.contains("cpu-limit")) {
        float limit = TimePeriod(j["cpu-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setCpuLimit(limit);
        log.log(V4_VVER, "Job #%i : CPU time limit %.3f CPU secs\n", id, limit);
    }
    job->setArrival(arrival);
    std::string file = j["file"].get<std::string>();
    
    // Translate dependencies (if any) to internal job IDs
    std::vector<int> idDependencies;
    std::vector<std::string> nameDependencies;
    if (j.contains("dependencies")) nameDependencies = j["dependencies"].get<std::vector<std::string>>();
    const std::string ending = ".json";
    for (auto name : nameDependencies) {
        // Convert to the name with ".json" file ending
        name += ending;
        // If the job is not yet known, assign to it a new ID
        // that will be used by the job later
        if (!_job_name_to_id.count(name)) {
            _job_name_to_id[name] = _running_id++;
            log.log(V3_VERB, "Forward mapping job \"%s\" to internal ID #%i\n", name.c_str(), _job_name_to_id[name]);
        }
        idDependencies.push_back(_job_name_to_id[name]);
    }

    // Callback to client: New job arrival.
    _new_job_callback(JobMetadata{std::shared_ptr<JobDescription>(job), file, idDependencies});
}

void JobFileAdapter::handleJobDone(const JobResult& result) {

    if (Terminator::isTerminating()) return;

    auto lock = _job_map_mutex.getLock();

    std::string eventFile = getJobFilePath(result.id, PENDING);
    if (!FileUtils::isRegularFile(eventFile)) {
        return; // File does not exist (any more)
    }
    std::ifstream i(eventFile);
    nlohmann::json j;
    try {
        i >> j;
    } catch (const nlohmann::detail::parse_error& e) {
        _logger.log(V1_WARN, "Parse error on %s: %s\n", eventFile.c_str(), e.what());
        return;
    }

    // Pack job result into JSON
    j["result"] = { 
        { "resultcode", result.result }, 
        { "resultstring", result.result == RESULT_SAT ? "SAT" : result.result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN" }, 
        { "revision", result.revision }, 
        { "solution", result.solution },
        { "responsetime", Timer::elapsedSeconds() - _job_id_to_image[result.id].arrivalTime }
    };

    // Remove file in "pending", move to "done"
    FileUtils::rm(eventFile);
    std::ofstream o(getJobFilePath(result.id, DONE));
    o << std::setw(4) << j << std::endl;
}

void JobFileAdapter::handleJobResultDeleted(const FileWatcher::Event& event, Logger& log) {

    if (Terminator::isTerminating()) return;

    log.log(V4_VVER, "Result file deletion event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    auto lock = _job_map_mutex.getLock();

    std::string jobName = event.name;
    jobName.erase(std::find(jobName.begin(), jobName.end(), '\0'), jobName.end());
    if (!_job_name_to_id.contains(jobName)) {
        log.log(V1_WARN, "Cannot clean up job \"%s\" : not known\n", jobName.c_str());
        return;
    }

    int id = _job_name_to_id.at(jobName);
    _job_name_to_id.erase(jobName);
    _job_id_to_image.erase(id);
}


std::string JobFileAdapter::getJobFilePath(int id, JobFileAdapter::Status status) {
    return _base_path + (status == NEW ? "/new/" : status == PENDING ? "/pending/" : "/done/") + _job_id_to_image[id].userQualifiedName;
}

std::string JobFileAdapter::getJobFilePath(const FileWatcher::Event& event, JobFileAdapter::Status status) {
    return _base_path + (status == NEW ? "/new/" : status == PENDING ? "/pending/" : "/done/") + event.name;
}

std::string JobFileAdapter::getUserFilePath(const std::string& user) {
    return _base_path + "/../users/" + user + ".json";
}
