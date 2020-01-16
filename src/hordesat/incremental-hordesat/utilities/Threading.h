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

#include "utilities/Logger.h"

#define TESTRUN(cmd, msg) int res = cmd; if (res != 0 && res != EPERM) {  printf(msg,res); kill(getpid(), SIGSEGV); }

class Mutex {
private:
	pthread_mutex_t mtx;
    
public:
	Mutex() {
		TESTRUN(pthread_mutex_init(&mtx, NULL), "Mutex init failed with msg %d\n")
	}
	virtual ~Mutex() {
		int res = pthread_mutex_destroy(&mtx); 
		if (res != 0) {  
			printf("Mutex destroy failed with msg %d\n",res); 
			//kill(getpid(), SIGSEGV);
		}
	}
	virtual void lock() {
		TESTRUN(pthread_mutex_lock(&mtx), "Mutex lock failed with msg %d\n")
	}
	virtual void unlock() {
		TESTRUN(pthread_mutex_unlock(&mtx), "Mutex unlock failed with msg %d\n")
	}
	bool tryLock() {
		// return true if lock acquired
		return pthread_mutex_trylock(&mtx) == 0;
	}
	pthread_mutex_t* mutex() {return &mtx;}
};

class ConditionVariable {
private:
    pthread_cond_t cond;
    
public:
    ConditionVariable() {
        TESTRUN(pthread_cond_init(&cond, NULL), "Condition var init failed with msg %d\n")
    }
    pthread_cond_t* get() {
        return &cond;
    }
	void wait(Mutex& mutex, bool& condition) {
		mutex.lock();
		while (!condition) {
			pthread_cond_wait(&cond, mutex.mutex());
		}
		mutex.unlock();
	}
	void notifyAll() {
		pthread_cond_broadcast(&cond);
	}
};

void setup_global_signal_handler(int signum);
void register_threadspecific_signal_handler (int signum, std::function<void()> func);
void ignore_signal (int signum);
void unignore_signal (int signum);

class Thread {
private:
	pthread_t thread;
public:
	Thread(void*(*method)(void*), void* arg) {
		pthread_create(&thread, NULL, method, arg);
	}
	void join() {
		pthread_join(thread, NULL);
	}
	int sendSignal(int signum) {
		return pthread_kill(thread, signum);
	}
	int detach() {
		return pthread_detach(thread);
	}
	void cancel() {
		pthread_cancel(thread);
	}
};



#endif /* THREADING_H_ */
