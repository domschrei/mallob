
#ifndef DOMPASCH_MALLOB_VERBOSE_MUTEX_H
#define DOMPASCH_MALLOB_VERBOSE_MUTEX_H

#include "utilities/Threading.h"
#include "util/console.h"

class VerboseMutex : public Mutex {

private:
    std::string name;

public:
    VerboseMutex() : Mutex() {
        this->name = "UNDEFINED";
    }
    VerboseMutex(const char* name) : Mutex() {
        this->name = std::string(name);
    }

    void lock() override {
        int microsecs = 10;
        bool locked = tryLock();
        while (!locked) {
            if (microsecs >= 10 * 1000) // 10 ms
                Console::log(Console::VVVERB, "waiting for mutex %s ...", name.c_str());
            usleep(microsecs);
            locked = tryLock();
            microsecs *= 2;
        }
        Console::log(Console::VVVERB, "locked %s", name.c_str());
    }
    void unlock() override {
        Mutex::unlock();
        Console::log(Console::VVVERB, "unlocked %s", name.c_str());
    }
};

#endif