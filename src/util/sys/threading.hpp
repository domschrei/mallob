
#ifndef DOMPASCH_MALLOB_THREADING_HPP
#define DOMPASCH_MALLOB_THREADING_HPP

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <optional>

#include "util/assert.hpp"

class Mutex {
private:
	std::mutex mtx;
    
public:
	void lock();
	void unlock();
	std::unique_lock<std::mutex> getLock();
	std::unique_lock<std::mutex> getTryLock();
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
	MutexLockedData(T& obj, std::unique_lock<std::mutex>&& lock) : _mtx(std::move(lock)), _obj(obj) {}
	bool locked() const {return _mtx.owns_lock();}
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
	[[nodiscard]] std::optional<MutexLockedData<T>> tryLock() {
		auto out = MutexLockedData<T>(_obj, _mtx.getTryLock());
		if (out.locked()) return out;
		return {};
	}
	T& getUnsafe() {
		return _obj;
	}
};

class Future {
public:
	template <typename T>
	static bool isValidAndReady(std::future<T>& fut) {
		if (!fut.valid()) return false;
		return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}
	template <typename T>
	static bool isPending(std::future<T>& fut) {
		return !isValidAndReady(fut);
	}
};


class SharedMutex {
private:
	 std::shared_mutex mtx;

public:
	// Exclusive (read-write) access
	std::unique_lock<std::shared_mutex> getUniqueLock() {return std::unique_lock(mtx);}
	// std::unique_lock<std::shared_mutex> getTryLock() {return std::unique_lock(mtx, std::try_to_lock);}

	// Shared (read-only) access: any number of holders, excludes exclusive holders
	std::shared_lock<std::shared_mutex> getSharedLock() { return std::shared_lock(mtx);}
	// std::shared_lock<std::shared_mutex> getTrySharedLock() {return std::shared_lock(mtx, std::try_to_lock);}
};


#endif 
