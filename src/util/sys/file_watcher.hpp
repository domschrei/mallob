
#ifndef DOMPASCH_MALLOB_FILE_WATCHER_HPP
#define DOMPASCH_MALLOB_FILE_WATCHER_HPP

#include <sys/inotify.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <dirent.h>

#include <string>
#include <thread>
#include <functional>
#include <memory>

#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/thread_group.hpp"
#include "util/logger.hpp"

class FileWatcher {

public:
    struct Event {
        uint32_t type;
        std::string name;
    };

    enum InitialFilesHandling { IGNORE, TRIGGER_CREATE_EVENT };

private:
    std::string _directory;
    int _inotify_fd = 0;
    int _inotify_wd = 0;
    std::thread _thread;
    bool _exiting;
    std::function<void(const Event&, Logger& logger)> _callback;
    InitialFilesHandling _init_files_handling;

public:
    FileWatcher() = default;
    FileWatcher(const std::string& directory, int events, 
        std::function<void(const Event&, Logger&)> callback, Logger& logger,
        InitialFilesHandling initFilesHandling = IGNORE, size_t numThreads = 1) : 
    
            _directory(directory),
            _exiting(false), _callback(callback), _init_files_handling(initFilesHandling) {
        
        _thread = std::thread([events, this, numThreads, &logger]() {

            FileUtils::mkdir(_directory);

            // Initialize inotify
            _inotify_fd = inotify_init();
            if (_inotify_fd < 0) {
                logger.log(V0_CRIT, "Failed to set up inotify, code %i\n", errno);
                logger.flush();
                abort();
            }
            
            // Make inotify nonblocking
            int flags = fcntl(_inotify_fd, F_GETFL, 0);
            fcntl(_inotify_fd, F_SETFL, flags | O_NONBLOCK);
            
            // Initialize watcher
            _inotify_wd = inotify_add_watch(_inotify_fd, _directory.c_str(), events);
            if (_inotify_wd < 0) {
                logger.log(V0_CRIT, "Failed to add inotify watch, code %i\n", errno);
                logger.flush();
                abort();
            }

            // Initialize thread group
            std::vector<Logger> loggers;
            for (size_t i = 0; i < numThreads; i++) {
                loggers.push_back(logger.copy(std::to_string(i), ""));
            }
            ThreadGroup<Logger> threads(numThreads, loggers);

            // Read job files which may already exist
            if (_init_files_handling == TRIGGER_CREATE_EVENT) {

                // Retrieve sorted list of files in directory
                std::vector<std::string> files;
                struct dirent* entry;
                DIR *dir = opendir(_directory.c_str());
                if (dir != NULL) {
                    while ((entry = readdir(dir)) != NULL) {
                        files.emplace_back(entry->d_name);
                    }
                    closedir(dir);
                } 
                sort(files.begin(), files.end());
                
                for (const auto& entry : files) {
                    const auto filenameStr = _directory + "/" + entry;
                    if (FileUtils::isRegularFile(filenameStr)) {
                        // Trigger CREATE event
                        //logger.log(V4_VVER, "FileWatcher: File event\n");
                        threads.doTask([this, entry] (Logger& lg) {
                            _callback(FileWatcher::Event{IN_CREATE, entry}, lg);
                        });
                    }
                    if (_exiting) return;
                }
            }
            
            // Main loop
            size_t eventSize = sizeof(struct inotify_event);
            size_t bufferSize = 1024 * eventSize + 16;
            char* buffer = (char*)malloc(bufferSize);
            while (!_exiting) {
                usleep(1000 * 10); // 10 milliseconds

                // poll for an event to occur
                int len = read(_inotify_fd, buffer, bufferSize);
                if (len == -1) continue;
                int i = 0;
                
                // Iterate over found events
                while (!_exiting && i < len) {
                    // digest event
                    inotify_event* event = (inotify_event*) &buffer[i];
                    Event ev{event->mask, std::string(event->name, event->len)};
                    //logger.log(V4_VVER, "FileWatcher: File event\n");
                    threads.doTask([this, ev](Logger& lg) {_callback(ev, lg);});
                    i += eventSize + event->len;
                }
            }

            free(buffer);
        });
    }

    ~FileWatcher() {
        _exiting = true;
        if (_thread.joinable()) _thread.join();
        if (_inotify_fd != 0) close(_inotify_fd);
    }
};

#endif
