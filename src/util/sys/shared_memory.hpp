
#ifndef DOMSCHREI_MALLOB_SHARED_MEMORY_H
#define DOMSCHREI_MALLOB_SHARED_MEMORY_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string>

namespace SharedMemory {
    
    enum AccessMode {READONLY, ARBITRARY};

    // From https://stackoverflow.com/a/5656561
    void* create(const std::string& specifier, size_t size);
    void* access(const std::string& specifier, size_t size, AccessMode accessMode = ARBITRARY);
    void close(char* addr, size_t size);
    void free(const std::string& specifier, char* addr, size_t size);
}

#endif