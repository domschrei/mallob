
#include <iomanip>

#include "job_file_adapter.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sat_reader.hpp"
#include "util/sys/time_period.hpp"
#include "util/random.hpp"

void JobFileAdapter::handleNewJob(const FileWatcher::Event& event) {

    Console::log(Console::VVERB, "New job file event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    auto lock = _job_map_mutex.getLock();

    // Attempt to read job file
    std::string eventFile = getJobFilePath(event, NEW);
    if (!std::filesystem::is_regular_file(eventFile)) {
        return; // File does not exist (any more)
    }
    nlohmann::json j;
    try {
        std::ifstream i(eventFile);
        i >> j;
    } catch (const nlohmann::detail::parse_error& e) {
        Console::log(Console::WARN, "Parse error on %s: %s\n", eventFile.c_str(), e.what());
        return;
    }

    // Check and read essential fields from JSON
    if (!j.contains("user") || !j.contains("name") || !j.contains("file")) {
        Console::log(Console::WARN, "Job file missing essential field(s). Ignoring this file.\n");
        return;
    }
    std::string user = j["user"].get<std::string>();
    std::string name = j["name"].get<std::string>();
    std::string jobName = user + "." + name + ".json";


    if (_job_name_to_id.count(jobName)) {
        Console::log(Console::WARN, "Modification of a file I already parsed! Ignoring.\n");
        return;
    }

    // Get user definition
    std::string userFile = getUserFilePath(user);
    nlohmann::json jUser;
    try {
        std::ifstream i(userFile);
        i >> jUser;
    } catch (const nlohmann::detail::parse_error& e) {
        Console::log(Console::WARN, "Unknown user or invalid user definition: %s\n", e.what());
        return;
    }
    if (!jUser.contains("id") || !jUser.contains("priority")) {
        Console::log(Console::WARN, "User file %s missing essential field(s). Ignoring job file with this user.\n", userFile.c_str());
        return;
    }
    if (jUser["id"].get<std::string>() != user) {
        Console::log(Console::WARN, "User file %s has inconsistent user ID. Ignoring job file with this user.\n", userFile.c_str());
        return;
    }
    float userPrio = jUser["priority"].get<float>();
    
    int id = _running_id++;

    // Parse CNF input file
    std::string file = j["file"].get<std::string>();
    SatReader r(file);
    VecPtr formula = r.read();
    VecPtr assumptions = std::make_shared<std::vector<int>>();
    if (formula == NULL) {
        Console::log(Console::WARN, "File %s could not be opened - skipping #%i", file.c_str(), id);
        return;
    }
    Console::log(Console::VERB, "%i literals including separation zeros, %i assumptions", formula->size(), assumptions->size());

    // Initialize new job
    float priority = userPrio * (j.contains("priority") ? j["priority"].get<float>() : 1.0f);
    if (_params.isNotNull("jjp")) {
        // Jitter job priority
        priority *= 0.99 + 0.01 * Random::rand();
    }
    float arrival = Timer::elapsedSeconds();
    JobDescription* job = new JobDescription(id, priority, /*incremental=*/false);
    if (j.contains("wallclock-limit")) {
        job->setWallclockLimit(TimePeriod(j["wallclock-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS));
        Console::log(Console::VVERB, "Job #%i : wallclock time limit %i secs\n", id, job->getWallclockLimit());
    }
    if (j.contains("cpu-limit")) {
        job->setCpuLimit(TimePeriod(j["cpu-limit"].get<std::string>()).get(TimePeriod::Unit::SECONDS));
        Console::log(Console::VVERB, "Job #%i : CPU time limit %i CPUs\n", id, job->getCpuLimit());
    }
    job->addPayload(formula);
    job->addAssumptions(assumptions);
    job->setNumVars(r.getNumVars());
    job->setArrival(arrival);

    // Callback to client: New job arrival.
    _new_job_callback(job);

    // Remember name/id mapping
    _job_name_to_id[jobName] = id;
    _job_id_to_image[id] = JobImage(id, jobName, event.name, arrival);

    // Remove original file, move to "pending"
    FileUtils::rm(eventFile);
    std::ofstream o(getJobFilePath(id, PENDING));
    o << std::setw(4) << j << std::endl;
}

void JobFileAdapter::handleJobDone(const JobResult& result) {

    auto lock = _job_map_mutex.getLock();

    std::string eventFile = getJobFilePath(result.id, PENDING);
    if (!std::filesystem::is_regular_file(eventFile)) {
        return; // File does not exist (any more)
    }
    std::ifstream i(eventFile);
    nlohmann::json j;
    try {
        i >> j;
    } catch (const nlohmann::detail::parse_error& e) {
        Console::log(Console::WARN, "Parse error on %s: %s\n", eventFile.c_str(), e.what());
        return;
    }

    // Pack job result into JSON
    j["result"] = { 
        { "resultcode", result.result }, 
        { "resultstring", result.result == 10 ? "SAT" : result.result == 20 ? "UNSAT" : "UNKNOWN" }, 
        { "revision", result.revision }, 
        { "solution", result.solution },
        { "responsetime", Timer::elapsedSeconds() - _job_id_to_image[result.id].arrivalTime }
    };

    // Remove file in "pending", move to "done"
    FileUtils::rm(eventFile);
    std::ofstream o(getJobFilePath(result.id, DONE));
    o << std::setw(4) << j << std::endl;
}

void JobFileAdapter::handleJobResultDeleted(const FileWatcher::Event& event) {

    Console::log(Console::VVERB, "Result file deletion event: type %i, name \"%s\"\n", event.type, event.name.c_str());

    auto lock = _job_map_mutex.getLock();

    std::string jobName = event.name;
    jobName.erase(std::find(jobName.begin(), jobName.end(), '\0'), jobName.end());
    if (!_job_name_to_id.contains(jobName)) {
        Console::log(Console::WARN, "Cannot clean up job \"%s\" : not known", jobName.c_str());
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
