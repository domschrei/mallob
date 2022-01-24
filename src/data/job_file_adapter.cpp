
#include <iomanip>
#include <fstream>

#include "job_file_adapter.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/time_period.hpp"
#include "util/random.hpp"
#include "app/sat/sat_constants.h"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"

void JobFileAdapter::handleNewJob(const FileWatcher::Event& event, Logger& log) {

    if (Terminator::isTerminating()) return;

    LOGGER(log, V3_VERB, "New job file event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    nlohmann::json j;
    std::string userFile, jobName;
    int id;
    float userPrio, arrival, priority;
    JobDescription::Application appl;

    {
        auto lock = _job_map_mutex.getLock();
        
        // Attempt to read job file
        std::string eventFile = getJobFilePath(event, NEW);
        if (!FileUtils::isRegularFile(eventFile)) {
            LOGGER(log, V3_VERB, "Job file %s does not exist (any more)\n", eventFile.c_str());        
            return; // File does not exist (any more)
        }
        try {
            std::ifstream i(eventFile);
            i >> j;
        } catch (const nlohmann::detail::parse_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Parse error on %s: %s\n", eventFile.c_str(), e.what());
            return;
        }

        // Check and read essential fields from JSON
        if (!j.contains("user") || !j.contains("name")) {
            LOGGER(log, V1_WARN, "[WARN] Job file missing essential field(s). Ignoring this file.\n");
            return;
        }
        std::string user = j["user"].get<std::string>();
        std::string name = j["name"].get<std::string>();
        jobName = user + "." + name + ".json";
        bool incremental = j.contains("incremental") ? j["incremental"].get<bool>() : false;

        // Get user definition
        userFile = getUserFilePath(user);
        nlohmann::json jUser;
        try {
            std::ifstream i(userFile);
            i >> jUser;
        } catch (const nlohmann::detail::parse_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Unknown user or invalid user definition: %s\n", e.what());
            return;
        }
        if (!jUser.contains("id") || !jUser.contains("priority")) {
            LOGGER(log, V1_WARN, "[WARN] User file %s missing essential field(s). Ignoring job file with this user.\n", userFile.c_str());
            return;
        }
        if (jUser["id"].get<std::string>() != user) {
            LOGGER(log, V1_WARN, "[WARN] User file %s has inconsistent user ID. Ignoring job file with this user.\n", userFile.c_str());
            return;
        }

        userPrio = jUser["priority"].get<float>();
        priority = userPrio * (j.contains("priority") ? j["priority"].get<float>() : 1.0f);
        if (_params.jitterJobPriorities()) {
            // Jitter job priority
            priority *= 0.99 + 0.01 * Random::rand();
        }
        appl = JobDescription::Application::DUMMY;
        if (j.contains("application")) {
            auto appStr = j["application"].get<std::string>();
            appl = appStr == "SAT" ? 
                (incremental ? JobDescription::Application::INCREMENTAL_SAT : JobDescription::Application::ONESHOT_SAT)
                : JobDescription::Application::DUMMY;
        }

        if (j.contains("interrupt") && j["interrupt"].get<bool>()) {
            if (!_job_name_to_id_rev.count(jobName)) {
                LOGGER(log, V1_WARN, "[WARN] Cannot interrupt unknown job \"%s\"\n", jobName.c_str());
                return;
            }
            auto [id, rev] = _job_name_to_id_rev.at(jobName);

            // Interrupt a job which is already present
            JobMetadata data;
            data.description = std::shared_ptr<JobDescription>(new JobDescription(id, 0, appl));
            data.interrupt = true;
            _new_job_callback(std::move(data));
            FileUtils::rm(eventFile);
            return;
        }

        arrival = j.contains("arrival") ? std::max(Timer::elapsedSeconds(), j["arrival"].get<float>()) : Timer::elapsedSeconds();

        if (incremental && j.contains("precursor")) {

            // This is a new increment of a former job - assign SAME internal ID
            auto precursorName = j["precursor"].get<std::string>() + ".json";
            if (!_job_name_to_id_rev.count(precursorName)) {
                LOGGER(log, V1_WARN, "[WARN] Unknown precursor job \"%s\"!\n", precursorName.c_str());
                return;
            }
            auto [jobId, rev] = _job_name_to_id_rev[precursorName];
            id = jobId;

            if (j.contains("done") && j["done"].get<bool>()) {

                // Incremental job is notified to be done
                LOGGER(log, V3_VERB, "Incremental job #%i is done\n", jobId);
                _job_name_to_id_rev.erase(precursorName);
                for (int rev = 0; rev <= _job_id_to_latest_rev[id]; rev++) {
                    _job_id_rev_to_image.erase(std::pair<int, int>(id, rev));
                }
                _job_id_to_latest_rev.erase(id);

                // Notify client that this incremental job is done
                JobMetadata data;
                data.description = std::shared_ptr<JobDescription>(new JobDescription(id, 0, appl));
                data.done = true;
                _new_job_callback(std::move(data));
                FileUtils::rm(eventFile);
                return;

            } else {

                // Job is not done -- add increment to job
                _job_id_to_latest_rev[id] = rev+1;
                _job_name_to_id_rev[jobName] = std::pair<int, int>(id, rev+1);
                JobImage img = JobImage(id, jobName, event.name, arrival);
                img.incremental = true;
                _job_id_rev_to_image[std::pair<int, int>(id, rev+1)] = img;
            }

        } else {

            // Create new internal ID for this job
            if (!_job_name_to_id_rev.count(jobName)) 
                _job_name_to_id_rev[jobName] = std::pair<int, int>(_running_id++, 0);
            auto pair = _job_name_to_id_rev[jobName];
            id = pair.first;
            LOGGER(log, V3_VERB, "Mapping job \"%s\" to internal ID #%i\n", jobName.c_str(), id);

            // Was job already parsed before?
            if (_job_id_rev_to_image.count(std::pair<int, int>(id, 0))) {
                LOGGER(log, V1_WARN, "[WARN] Modification of a file I already parsed! Ignoring.\n");
                return;
            }

            JobImage img(id, jobName, event.name, arrival);
            img.incremental = incremental;
            _job_id_rev_to_image[std::pair<int, int>(id, 0)] = std::move(img);
            _job_id_to_latest_rev[id] = 0;
        }

        // Remove original file, move to "pending"
        {
            std::string pendingFile = getJobFilePath(id, _job_id_to_latest_rev[id], PENDING);
            LOGGER(log, V4_VVER, "Move %s to %s\n", eventFile.c_str(), pendingFile.c_str());
            std::ofstream o(pendingFile);
            o << std::setw(4) << j << std::endl;
        }
        {
            std::ofstream o(getJobFilePath(id, _job_id_to_latest_rev[id], INTRODUCED));
            o << std::setw(4) << j << std::endl;
        }
        FileUtils::rm(eventFile);
    }

    // Initialize new job
    JobDescription* job = new JobDescription(id, priority, appl);
    job->setRevision(_job_id_to_latest_rev[id]);
    if (j.contains("wallclock-limit")) {
        float limit = TimePeriod(j["wallclock-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setWallclockLimit(limit);
        LOGGER(log, V4_VVER, "Job #%i : wallclock time limit %.3f secs\n", id, limit);
    }
    if (j.contains("cpu-limit")) {
        float limit = TimePeriod(j["cpu-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS);
        job->setCpuLimit(limit);
        LOGGER(log, V4_VVER, "Job #%i : CPU time limit %.3f CPU secs\n", id, limit);
    }
    if (j.contains("max-demand")) {
        int maxDemand = j["max-demand"].get<int>();
        job->setMaxDemand(maxDemand);
        LOGGER(log, V4_VVER, "Job #%i : max demand %i\n", id, maxDemand);
    }
    if (j.contains("assumptions")) {
        job->setPreloadedAssumptions(j["assumptions"].get<std::vector<int>>());
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
        auto lock = _job_map_mutex.getLock();
        if (!_job_name_to_id_rev.count(name)) {
            _job_name_to_id_rev[name] = std::pair<int, int>(_running_id++, 0);
            LOGGER(log, V3_VERB, "Forward mapping job \"%s\" to internal ID #%i\n", name.c_str(), _job_name_to_id_rev[name].first);
        }
        idDependencies.push_back(_job_name_to_id_rev[name].first); // TODO inexact: introduce dependencies for job revisions
    }

    // Callback to client: New job arrival.
    _new_job_callback(JobMetadata{std::shared_ptr<JobDescription>(job), file, idDependencies});
}

void JobFileAdapter::handleJobDone(const JobResult& result, const JobDescription::Statistics& stats) {

    if (Terminator::isTerminating()) return;

    auto lock = _job_map_mutex.getLock();

    std::string eventFile = getJobFilePath(result.id, result.revision, PENDING);
    LOGGER(_logger, V3_VERB, "Job done event for #%i rev. %i : %s\n", result.id, result.revision, eventFile.c_str());

    if (!FileUtils::isRegularFile(eventFile)) {
        LOGGER(_logger, V1_WARN, "[WARN] Pending job file %s gone!\n", eventFile.c_str());
        return; // File does not exist (any more)
    }
    std::ifstream i(eventFile);
    nlohmann::json j;
    try {
        i >> j;
    } catch (const nlohmann::detail::parse_error& e) {
        LOGGER(_logger, V1_WARN, "[WARN] Parse error on %s: %s\n", eventFile.c_str(), e.what());
        return;
    }
    
    auto& img = _job_id_rev_to_image[std::pair<int, int>(result.id, result.revision)];

    // Pack job result into JSON
    j["internal_id"] = result.id;
    j["internal_revision"] = result.revision;
    j["result"] = { 
        { "resultcode", result.result }, 
        { "resultstring", result.result == RESULT_SAT ? "SAT" : result.result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN" }, 
        { "solution", result.solution },
    };
    j["stats"] = {
        { "time", {
            { "parsing", stats.parseTime },
            { "scheduling", stats.schedulingTime },
            { "first_balancing_latency", stats.latencyOf1stVolumeUpdate },
            { "processing", stats.processingTime },
            { "total", Timer::elapsedSeconds() - img.arrivalTime }
        } },
        { "used_wallclock_seconds" , stats.usedWallclockSeconds },
        { "used_cpu_seconds" , stats.usedCpuSeconds }
    };

    // Remove file in "pending", move to "done"
    std::ofstream o(getJobFilePath(result.id, result.revision, DONE));
    o << std::setw(4) << j << std::endl;
    FileUtils::rm(eventFile);
}

void JobFileAdapter::handleJobResultDeleted(const FileWatcher::Event& event, Logger& log) {

    if (Terminator::isTerminating()) return;

    LOGGER(log, V4_VVER, "Result file deletion event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    auto lock = _job_map_mutex.getLock();

    std::string jobName = event.name;
    jobName.erase(std::find(jobName.begin(), jobName.end(), '\0'), jobName.end());
    if (!_job_name_to_id_rev.contains(jobName)) {
        LOGGER(log, V1_WARN, "[WARN] Cannot clean up job \"%s\" : not known\n", jobName.c_str());
        return;
    }

    if (_job_id_rev_to_image[_job_name_to_id_rev.at(jobName)].incremental)
        return; // do not clean up

    auto [id, rev] = _job_name_to_id_rev.at(jobName);
    _job_name_to_id_rev.erase(jobName);
    _job_id_rev_to_image.erase(std::pair<int, int>(id, rev));
    LOGGER(log, V4_VVER, "Cleaned up \"%s\"\n", event.name.c_str());
}

std::string getDirectory(JobFileAdapter::Status status) {
    return status == JobFileAdapter::Status::NEW ? "/new/" : (
        status == JobFileAdapter::Status::PENDING ? "/pending/" : (
            status == JobFileAdapter::Status::INTRODUCED ? "/introduced/" : "/done/"
        )
    );
}

std::string JobFileAdapter::getJobFilePath(int id, int revision, JobFileAdapter::Status status) {
    return _base_path + getDirectory(status) + 
        _job_id_rev_to_image[std::pair<int, int>(id, revision)].userQualifiedName;
}

std::string JobFileAdapter::getJobFilePath(const FileWatcher::Event& event, JobFileAdapter::Status status) {
    return _base_path + getDirectory(status) + event.name;
}

std::string JobFileAdapter::getUserFilePath(const std::string& user) {
    return _base_path + "/../users/" + user + ".json";
}
