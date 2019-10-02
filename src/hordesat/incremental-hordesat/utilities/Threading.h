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

#define TESTRUN(cmd, msg) int res = cmd; if (res != 0) { printf(msg,res); exit(res); }

class Mutex {
private:
	pthread_mutex_t mtx;
    
public:
	Mutex() {
		TESTRUN(pthread_mutex_init(&mtx, NULL), "Mutex init failed with msg %d\n")
	}
	virtual ~Mutex() {
		TESTRUN(pthread_mutex_destroy(&mtx), "Mutex destroy failed with msg %d\n")
	}
	void lock() {
		TESTRUN(pthread_mutex_lock(&mtx), "Mutex lock failed with msg %d\n")
	}
	void unlock() {
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
};

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
};



#endif /* THREADING_H_ */
