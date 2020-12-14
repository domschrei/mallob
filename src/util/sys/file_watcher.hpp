
#ifndef DOMPASCH_MALLOB_FILE_WATCHER_HPP
#define DOMPASCH_MALLOB_FILE_WATCHER_HPP

#include <sys/inotify.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

#include <string>
#include <thread>
#include <functional>
#include <filesystem>

#include "util/console.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/thread_group.hpp"

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
    std::function<void(const Event&)> _callback;
    InitialFilesHandling _init_files_handling;

public:
    FileWatcher() = default;
    FileWatcher(const std::string& directory, int events, std::function<void(const Event&)> callback, 
    InitialFilesHandling initFilesHandling = IGNORE) : 
            _directory(directory),
            _exiting(false), _callback(callback), _init_files_handling(initFilesHandling) {
        
        _thread = std::thread([events, this]() {

            FileUtils::mkdir(_directory);

            ThreadGroup threads;

            // Read job files which may already exist
            if (_init_files_handling == TRIGGER_CREATE_EVENT) {
                const std::filesystem::path newJobsPath { _directory };
                for (const auto& entry : std::filesystem::directory_iterator(newJobsPath)) {
                    const auto filenameStr = entry.path().filename().string();
                    if (entry.is_regular_file()) {
                        // Trigger CREATE event
                        Console::log(Console::VVERB, "FileWatcher: File event");
                        threads.doTask([this, filenameStr]() {_callback(FileWatcher::Event{IN_CREATE, filenameStr});});
                    }
                    if (_exiting) return;
                }
            }
            
            // Initialize inotify
            _inotify_fd = inotify_init();
            if (_inotify_fd < 0) {
                Console::log(Console::CRIT, "Failed to set up inotify, code %i\n", errno);
                abort();
            }
            
            // Make inotify nonblocking
            int flags = fcntl(_inotify_fd, F_GETFL, 0);
            fcntl(_inotify_fd, F_SETFL, flags | O_NONBLOCK);
            
            // Initialize watcher
            _inotify_wd = inotify_add_watch(_inotify_fd, _directory.c_str(), events);
            if (_inotify_wd < 0) {
                Console::log(Console::CRIT, "Failed to add inotify watch, code %i\n", errno);
                abort();
            }

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
                while (i < len) {
                    // digest event
                    inotify_event* event = (inotify_event*) &buffer[i];
                    Event ev{event->mask, std::string(event->name, event->len)};
                    Console::log(Console::VVERB, "FileWatcher: File event");
                    threads.doTask([this, ev]() {_callback(ev);});
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
