/*
 * Threading.h
 *
 *  Created on: Nov 25, 2014
 *      Author: balyo
 */

#ifndef THREADING_H_
#define THREADING_H_

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>
#include <unistd.h>
#include <signal.h>

#include <functional>
#include <mutex>
#include <condition_variable>

#define TESTRUN(cmd, msg) int res = cmd; if (res != 0) {  printf(msg,res); kill(getpid(), SIGSEGV); }

class Mutex {
private:
	//pthread_mutex_t mtx;
	std::mutex mtx;
    
public:
	Mutex() {
		//TESTRUN(pthread_mutex_init(&mtx, NULL), "Mutex init failed with msg %d\n")
	}
	virtual ~Mutex() {
		//TESTRUN(pthread_mutex_destroy(&mtx), "Mutex destroy failed with msg %d\n")
	}
	virtual void lock() {
		//TESTRUN(pthread_mutex_lock(&mtx), "Mutex lock failed with msg %d\n")
		mtx.lock();
	}
	virtual void unlock() {
		//TESTRUN(pthread_mutex_unlock(&mtx), "Mutex unlock failed with msg %d\n")
		mtx.unlock();
	}
	std::unique_lock<std::mutex> getLock() {
		return std::unique_lock<std::mutex>(mtx);
	}
	bool tryLock() {
		// return true if lock acquired
		//return pthread_mutex_trylock(&mtx) == 0;
		return mtx.try_lock();
	}
	//pthread_mutex_t* mutex() {return &mtx;}
};

class ConditionVariable {
private:
	std::condition_variable condvar;
    //pthread_cond_t cond;
    
public:
	ConditionVariable() {
        //TESTRUN(pthread_cond_init(&cond, NULL), "Condition var init failed with msg %d\n")
    }
    void wait(Mutex& mutex, std::function<bool()> condition) {
		auto lock = mutex.getLock();
		while (!condition()) condvar.wait(lock);
	}
	// Pass unique_lock if already in use
    void wait(std::unique_lock<std::mutex>& lock, std::function<bool()> condition) {
		while (!condition()) condvar.wait(lock);
	}
	void notify() {
		condvar.notify_all();
	}
};

class SharedMemMutex {
private:
	pthread_mutex_t* mtx = NULL;
	pthread_mutexattr_t attrmutex;

public:
	static size_t getSharedMemorySize() {
		return sizeof(pthread_mutex_t);
	}
	SharedMemMutex(void* memoryLocation) {

		/* Initialise attribute to mutex. */
		pthread_mutexattr_init(&attrmutex);
		pthread_mutexattr_setpshared(&attrmutex, PTHREAD_PROCESS_SHARED);

		/* Point mutex to shared memory. */
		mtx = (pthread_mutex_t*)memoryLocation;

		/* Initialise mutex. */
		pthread_mutex_init(mtx, &attrmutex);
	}
	~SharedMemMutex() {
		/* Clean up. */
		pthread_mutex_destroy(mtx);
		pthread_mutexattr_destroy(&attrmutex); 
	}

	void lock() {
		pthread_mutex_lock(mtx);
	}
	void unlock() {
		pthread_mutex_unlock(mtx);
	}
	pthread_mutex_t* getHandle() {
		return mtx;
	}
};

class SharedMemConditionVariable {
private:
	pthread_cond_t* pcond = NULL;
	pthread_condattr_t attrcond;

public:
	static size_t getSharedMemorySize() {
		return sizeof(pthread_cond_t);
	}
	SharedMemConditionVariable(void* memoryLocation) {

		/* Initialise attribute to condition. */
		pthread_condattr_init(&attrcond);
		pthread_condattr_setpshared(&attrcond, PTHREAD_PROCESS_SHARED);

		/* Point pcond to shared memory. */
		pcond = (pthread_cond_t*) memoryLocation;

		/* Initialise condition. */
		pthread_cond_init(pcond, &attrcond);
	}
	~SharedMemConditionVariable() {
		/* Clean up. */
		pthread_cond_destroy(pcond);
		pthread_condattr_destroy(&attrcond); 
	}

	void wait(SharedMemMutex& mutex, std::function<bool()> condition) {
		mutex.lock();
		while (!condition()) pthread_cond_wait(pcond, mutex.getHandle());
		mutex.unlock();
	}
	bool timedWait(SharedMemMutex& mutex, std::function<bool()> condition, long nsecs) {
		mutex.lock();
		timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
    	ts.tv_nsec += nsecs; 
		int res = 0;
		while (!condition() && res == 0) res = pthread_cond_timedwait(pcond, mutex.getHandle(), &ts);
		mutex.unlock();
		return res;
	}
	void notify() {
		pthread_cond_broadcast(pcond);
	}

};

#endif 
