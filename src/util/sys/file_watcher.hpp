
#ifndef DOMPASCH_MALLOB_FILE_WATCHER_HPP
#define DOMPASCH_MALLOB_FILE_WATCHER_HPP

#include <sys/inotify.h>
#include <unistd.h>
#include <assert.h>

#include <string>
#include <thread>
#include <functional>

#include "util/console.hpp"
#include "util/sys/fileutils.hpp"

class FileWatcher {

public:
    struct Event {
        uint32_t type;
        std::string name;
    };

private:
    std::string _directory;
    int _inotify_fd = 0;
    int _inotify_wd = 0;
    std::thread _thread;
    bool _exiting;
    std::function<void(const Event&)> _callback;

public:
    FileWatcher() = default;
    FileWatcher(const std::string& directory, int events, std::function<void(const Event&)> callback) : 
            _directory(directory),
            _exiting(false), _callback(callback) {
        
        FileUtils::mkdir(_directory);

        _inotify_fd = inotify_init();
        if (_inotify_fd < 0) {
            Console::log(Console::CRIT, "Failed to set up inotify, code %i\n", errno);
            abort();
        }
        _inotify_wd = inotify_add_watch(_inotify_fd, _directory.c_str(), events);
        if (_inotify_wd < 0) {
            Console::log(Console::CRIT, "Failed to add inotify watch, code %i\n", errno);
            abort();
        }
        
        _thread = std::thread([&]() {
            size_t eventSize = sizeof(struct inotify_event);
            size_t bufferSize = 1024 * eventSize + 16;
            char* buffer = (char*)malloc(bufferSize);

            while (!_exiting) {
                // wait for an event to occur
                int len = read(_inotify_fd, buffer, bufferSize);
                int i = 0;
                assert(len >= 0);
                
                // Iterate over found events
                while (i < len) {
                    // digest event
                    inotify_event* event = (inotify_event*) &buffer[i];
                    Event ev{event->mask, std::string(event->name, event->len)};
                    _callback(ev);
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
