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

#include "utilities/Logger.h"

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
	void notify() {
		condvar.notify_all();
	}
};


#endif /* THREADING_H_ */
