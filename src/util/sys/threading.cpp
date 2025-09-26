
#include "threading.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <condition_variable>

void Mutex::lock() {
    mtx.lock();
}
void Mutex::unlock() {
    mtx.unlock();
}
std::unique_lock<std::mutex> Mutex::getLock() {
    return std::unique_lock<std::mutex>(mtx);
}
std::unique_lock<std::mutex> Mutex::getTryLock() {
    return std::unique_lock<std::mutex>(mtx, std::try_to_lock_t());
}
bool Mutex::tryLock() {
    // return true if lock acquired
    return mtx.try_lock();
}

void ConditionVariable::wait(Mutex& mutex, std::function<bool()> condition) {
    auto lock = mutex.getLock();
    while (!condition()) condvar.wait(lock);
}
void ConditionVariable::waitWithTimeout(Mutex& mutex, int millisecs, std::function<bool()> condition) {
    auto lock = mutex.getLock();
    while (!condition()) condvar.wait_for(lock, std::chrono::milliseconds(millisecs));
}
void ConditionVariable::waitWithLockedMutex(std::unique_lock<std::mutex>& lock, std::function<bool()> condition) {
    while (!condition()) condvar.wait(lock);
}
void ConditionVariable::notifySingle() {
    condvar.notify_one();
}
void ConditionVariable::notify() {
    condvar.notify_all();
}
