
#include "shared_memory.hpp"
#include "util/logger.hpp"

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>

//tsl::robin_map<const void*, size_t> shmem_to_size;

namespace SharedMemory {

    void* create(const std::string& specifier, size_t size) {

        int memFd = shm_open(specifier.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRWXU);
        if (memFd == -1) return nullptr;

        int res = ftruncate(memFd, size);
        if (res == -1) {
            ::close(memFd);
            return nullptr;
        }

        void* buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
        ::close(memFd);

        //shmem_to_size[buffer] = size;
        //if (size>0) LOG(V2_INFO, "SHMEMMAP CREATE %p %lu %s\n", buffer, size, specifier.c_str());
        return buffer;
    }

    void* access(const std::string& specifier, size_t size, AccessMode accessMode) {
        if (size == 0) return (void*) 1;

        std::string shmemFile = "/dev/shm/" + specifier;
        if (::access(shmemFile.c_str(), F_OK) == -1) return nullptr;

        auto oflag = accessMode == READONLY ? O_RDONLY : O_RDWR;
        int memFd = shm_open(specifier.c_str(), oflag, 0);
        if (memFd == -1) return nullptr;

        auto prot = accessMode == READONLY ? PROT_READ : (PROT_READ | PROT_WRITE);
        void* buffer = mmap(NULL, size, prot, MAP_SHARED, memFd, 0);
        ::close(memFd);

        return buffer;
    }

    void close(char* addr, size_t size) {
        if (size == 0) return;
        munmap(addr, size);
    }

    void free(const std::string& specifier, char* addr, size_t size) {
        if (size == 0) return;
        assert(addr);
        //LOG(V2_INFO, "SHMEMMAP FREE %p %lu %s\n", addr, size, specifier.c_str());
        //if (shmem_to_size.count(addr)) assert(shmem_to_size[addr] == size);
        //shmem_to_size.erase(addr);
        munmap(addr, size);
        shm_unlink(specifier.c_str());
    }
}