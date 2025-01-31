
#ifndef DOMPASCH_MALLOB_BACKGROUND_WORKER_HPP
#define DOMPASCH_MALLOB_BACKGROUND_WORKER_HPP

#include <thread>
#include <functional>
#include <signal.h>

#include "util/sys/terminator.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

class BackgroundWorker {

private:
    volatile bool _terminate = false;
    std::thread _thread;

public:
    BackgroundWorker() {}
    void run(std::function<void()> runnable) {
        _terminate = false;
        _thread = std::thread(runnable);
    }
    bool continueRunning() const {
        return !_terminate;
    }
    bool isRunning() const {
        return _thread.joinable();
    }
    void stop() {
        _terminate = true;
        join();
    }
    void stopWithoutWaiting() {
        _terminate = true;
    }
    void join() {
        if (_thread.joinable()) _thread.join();
    }
    ~BackgroundWorker() {
        stop();
    }
};

#endif
