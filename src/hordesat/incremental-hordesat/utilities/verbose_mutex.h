
#ifndef DOMPASCH_MALLOB_VERBOSE_MUTEX_H
#define DOMPASCH_MALLOB_VERBOSE_MUTEX_H

#include "utilities/Threading.h"

class VerboseMutex : public Mutex {

private:
    std::string name;
    void (*logCallback)(const char*);

public:
    VerboseMutex() : Mutex() {
        this->name = "UNDEFINED";
        this->logCallback = NULL;
    }
    VerboseMutex(const char* name, void (*logCallback)(const char*)) : Mutex() {
        this->name = std::string(name);
        this->logCallback = logCallback;
    }

    void lock() override {
        int microsecs = 10;
        bool locked = tryLock();
        while (!locked) {
            if (microsecs >= 10 * 1000) {
                std::string msgStr = std::string("waiting for ") + name;
                const char* msg = msgStr.c_str();
                if (logCallback != NULL) logCallback(msg);
                else log(2, msg);
            }
            usleep(microsecs);
            locked = tryLock();
            microsecs *= 2;
        }
        std::string msgStr = std::string("locked ") + name;
        const char* msg = msgStr.c_str();
        if (logCallback != NULL) logCallback(msg);
        else log(2, msg);
    }
    void unlock() override {
        Mutex::unlock();
        std::string msgStr = std::string("unlocked ") + name;
        const char* msg = msgStr.c_str();
        if (logCallback != NULL) logCallback(msg);
        else log(2, msg);
    }
};

#endif