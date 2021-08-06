
#ifndef DOMPASCH_MALLOB_THREADING_HPP
#define DOMPASCH_MALLOB_THREADING_HPP

#include <functional>
#include <mutex>
#include <condition_variable>

class Mutex {
private:
	std::mutex mtx;
    
public:
	void lock();
	void unlock();
	std::unique_lock<std::mutex> getLock();
	bool tryLock();
};

class ConditionVariable {
private:
	std::condition_variable condvar;
    
public:
    void wait(Mutex& mutex, std::function<bool()> condition);
	void waitWithLockedMutex(std::unique_lock<std::mutex>& lock, std::function<bool()> condition);
	void notifySingle();
	void notify();
};

#endif 
