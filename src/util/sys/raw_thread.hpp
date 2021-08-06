
#ifndef DOMPASCH_MALLOB_RAW_THREAD_HPP
#define DOMPASCH_MALLOB_RAW_THREAD_HPP

#include <functional>

#include "util/sys/threading.hpp"

#define RAW_THREAD_STACK_SIZE 1024

int raw_thread_exec(void* arg);

class RawThread {

public:
    struct ThreadState {
        std::function<void()> runnable;
        int* stack = nullptr;
        bool returned = false;
        Mutex mutex;
        ConditionVariable condVar;

        ~ThreadState();
        int run();
    };
    
private:
    pid_t _pid = -1;
    ThreadState* _state = nullptr;

public: 
    RawThread() {}
    RawThread(std::function<void()> runnable);
    RawThread(RawThread&& other);
    RawThread(RawThread& other) = delete;
    RawThread& operator=(RawThread& other) = delete;
    RawThread& operator=(RawThread&& other);
    ~RawThread();

    bool joinable() const;
    bool exited() const;
    void join();
};

#endif
