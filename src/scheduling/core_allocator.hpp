
#pragma once

#include "util/assert.hpp"
#include "util/logger.hpp"

class CoreAllocator {

private:
    int _nb_available {0};

public:
    CoreAllocator(int nbThreads) : _nb_available(nbThreads) {}
    int requestCores(int nbRequested) {
        int nbGranted;
        if (nbRequested > _nb_available) {
            nbGranted = _nb_available;
            _nb_available = 0;
        } else {
            nbGranted = nbRequested;
            _nb_available -= nbRequested;
        }
        LOG(V2_INFO, "COREALLOC req %i -> granted %i, available %i\n", nbRequested, nbGranted, _nb_available);
        return nbGranted;
    }
    void returnCores(int nbReturned) {
        _nb_available += nbReturned;
        LOG(V2_INFO, "COREALLOC ret %i -> available %i\n", nbReturned, _nb_available);
    }
};

class ProcessWideCoreAllocator {
private:
    static CoreAllocator* ca;
public:
    static void init(int nbThreads) {ca = new CoreAllocator(nbThreads);}
    static CoreAllocator& get() {
        assert(ca);
        return *ca;
    }  
};
