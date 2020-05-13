
#ifndef DOMSCHREI_MALLOB_SHARED_MEMORY_H
#define DOMSCHREI_MALLOB_SHARED_MEMORY_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

namespace SharedMemory {
    // From https://stackoverflow.com/a/5656561
    void* create(size_t size);
}

#endif