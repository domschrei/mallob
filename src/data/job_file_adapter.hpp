
#ifndef DOMSCHREI_MALLOB_JOB_FILE_LISTENER_HPP
#define DOMSCHREI_MALLOB_JOB_FILE_LISTENER_HPP

#include <functional>

#include "util/sys/file_watcher.hpp"
#include "util/console.hpp"
#include "util/robin_hood.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "util/json.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/threading.hpp"

class JobFileAdapter {

public:
    enum Status {NEW, PENDING, DONE};
    struct JobImage {
        int id;
        std::string userQualifiedName;
        std::string originalFileName;
        float arrivalTime;

        JobImage() = default;
        JobImage(int id, const std::string& userQualifiedName, const std::string& originalFileName, float arrivalTime) 
            : id(id), userQualifiedName(userQualifiedName), originalFileName(originalFileName), arrivalTime(arrivalTime) {} 
    };

private:
    const Parameters& _params;

    std::string _base_path;
    FileWatcher _new_jobs_watcher;
    FileWatcher _results_watcher;
    robin_hood::unordered_node_map<std::string, int> _job_name_to_id;
    robin_hood::unordered_node_map<int, JobImage> _job_id_to_image;
    int _running_id;

    std::function<void(std::shared_ptr<JobDescription> desc)> _new_job_callback;
    Mutex _job_map_mutex;

public:

    JobFileAdapter(const Parameters& params, const std::string& basePath, 
                        std::function<void(std::shared_ptr<JobDescription> desc)> newJobCallback) : 
        _params(params),
        _base_path(basePath),
        _new_jobs_watcher(_base_path + "/new/", (int) (IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE), 
            [&](const FileWatcher::Event& event) {handleNewJob(event);}, 
            FileWatcher::InitialFilesHandling::TRIGGER_CREATE_EVENT),
        _results_watcher(_base_path + "/done/", (int) (IN_DELETE | IN_MOVED_FROM), 
            [&](const FileWatcher::Event& event) {handleJobResultDeleted(event);}),
        _running_id(1), _new_job_callback(newJobCallback) {
        
        // Create directories as necessary
        FileUtils::mkdir(_base_path + "/new/");
        FileUtils::mkdir(_base_path + "/pending/");
        FileUtils::mkdir(_base_path + "/done/");
    }

    // FileWatcher events
    void handleNewJob(const FileWatcher::Event& event);
    void handleJobResultDeleted(const FileWatcher::Event& event);

    // Event when a job is finished
    void handleJobDone(const JobResult& result);

private:
    std::string getJobFilePath(int id, Status status);
    std::string getJobFilePath(const FileWatcher::Event& event, Status status);
    std::string getUserFilePath(const std::string& user);

};

#endif