
#ifndef DOMPASCH_MALLOB_THREADING_HPP
#define DOMPASCH_MALLOB_THREADING_HPP

#include <functional>
#include <mutex>
#include <condition_variable>

#include "util/assert.hpp"

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
	void waitWithTimeout(Mutex& mutex, int millisecs, std::function<bool()> condition);
	void waitWithLockedMutex(std::unique_lock<std::mutex>& lock, std::function<bool()> condition);
	void notifySingle();
	void notify();
};

template <typename T>
class MutexLockedData {
private:
	std::unique_lock<std::mutex> _mtx;
	T& _obj;
public:
	MutexLockedData(T& obj, Mutex& mtx) : _mtx(mtx.getLock()), _obj(obj) {}
	void unlock() {if (_mtx.owns_lock()) _mtx.unlock();}
	T& operator*() {assert(_mtx.owns_lock()); return _obj;}
	T* operator->() {assert(_mtx.owns_lock()); return &_obj;}
	T& get() {return _obj;}
};

template <typename T>
class GuardedData {
private:
	Mutex _mtx;
	T _obj;
public:
	GuardedData() {}
	GuardedData(T&& obj) : _obj(std::move(obj)) {}
	[[nodiscard]] MutexLockedData<T> lock() {
		return MutexLockedData<T>(_obj, _mtx);
	}
};

#endif 
