
#ifndef DOMSCHREI_MALLOB_JOB_FILE_LISTENER_HPP
#define DOMSCHREI_MALLOB_JOB_FILE_LISTENER_HPP

#include <functional>
#include <filesystem>

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
    std::string _base_path;
    FileWatcher _new_jobs_watcher;
    FileWatcher _results_watcher;
    robin_hood::unordered_node_map<std::string, int> _job_name_to_id;
    robin_hood::unordered_node_map<int, JobImage> _job_id_to_image;
    int _running_id;

    std::function<void(JobDescription* desc)> _new_job_callback;
    Mutex _job_map_mutex;

public:

    JobFileAdapter() = default;
    JobFileAdapter(const std::string& basePath, std::function<void(JobDescription* desc)> newJobCallback) : 
        _base_path(basePath),
        _new_jobs_watcher(_base_path + "/new/", (int) (IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE), 
            [&](const FileWatcher::Event& event) {handleNewJob(event);}),
        _results_watcher(_base_path + "/done/", (int) (IN_DELETE | IN_MOVED_FROM), 
            [&](const FileWatcher::Event& event) {handleJobResultDeleted(event);}),
        _running_id(1), _new_job_callback(newJobCallback) {

        // Read job files which already exist
        const std::filesystem::path newJobsPath { _base_path + "/new/" };
        for (const auto& entry : std::filesystem::directory_iterator(newJobsPath)) {
            const auto filenameStr = entry.path().filename().string();
            if (entry.is_regular_file()) {
                handleNewJob(FileWatcher::Event{IN_CREATE, filenameStr});
            }
        }
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