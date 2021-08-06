
#include "raw_thread.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

int RawThread::ThreadState::run() {
    
    runnable();

    {
        auto lock = mutex.getLock();
        returned = true;
    }
    condVar.notify();
    return 0;
}

RawThread::ThreadState::~ThreadState() {
    if (stack != nullptr) free(stack);
}

RawThread::RawThread(std::function<void()> runnable) {
    _state = new ThreadState();
    _state->runnable = runnable;
    _state->stack = (int*)malloc(RAW_THREAD_STACK_SIZE*sizeof(int));
    _state->returned = false;
    _pid = clone(&raw_thread_exec, _state->stack+RAW_THREAD_STACK_SIZE, 
        CLONE_VM
        |CLONE_FS
        |CLONE_FILES
        |CLONE_SIGHAND
        |CLONE_THREAD
        |CLONE_SYSVSEM
    , _state);
}
RawThread::RawThread(RawThread&& other) {
    _state = other._state;
    _pid = other._pid;
    other._state = nullptr;
    other._pid = -1;
}
RawThread& RawThread::operator=(RawThread&& other) {
    _pid = other._pid;
    _state = other._state;
    other._pid = -1;
    other._state = nullptr;
    return *this;
}

RawThread::~RawThread() {
    if (_state != nullptr) {
        join();
        delete _state;
    }
}

bool RawThread::joinable() const {
    return _pid != -1 && _state != nullptr;
}
bool RawThread::exited() const {
    return _state->returned;
}
void RawThread::join() {
    if (_state == nullptr) abort();
    _state->condVar.wait(_state->mutex, [&]() {return _state->returned;});
}

int raw_thread_exec(void* arg) {
    return ((RawThread::ThreadState*)arg)->run();
}
