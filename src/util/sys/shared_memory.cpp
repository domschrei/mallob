
#include "shared_memory.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "util/assert.hpp"

namespace SharedMemory {

    void* create(const std::string& specifier, size_t size) {

        int memFd = shm_open(specifier.c_str(), O_CREAT | O_RDWR, S_IRWXU);
        assert(memFd != -1);

        int res = ftruncate(memFd, size);
        assert(res != -1);

        // If compiled without assert
        if (memFd == -1 || res == -1) abort();

        void *buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
        assert(buffer != nullptr);
        close(memFd);

        return buffer;
    }

    bool canAccess(const std::string& specifier) {
        std::string shmemFile = "/dev/shm/" + specifier;
        return ::access(shmemFile.c_str(), F_OK) != -1;
    }

    void* access(const std::string& specifier, size_t size) {

        std::string shmemFile = "/dev/shm/" + specifier;
        if (::access(shmemFile.c_str(), F_OK) == -1) return nullptr;

        int memFd = shm_open(specifier.c_str(), O_RDWR, 0);
        if (memFd == -1) {
            perror("Can't open file");
            return nullptr;
        }

        void *buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
        if (buffer == NULL) {
            perror("Can't mmap");
            return nullptr;
        }
        close(memFd);

        return buffer;
    }

    void free(const std::string& specifier, char* addr, size_t size) {
        munmap(addr, size);
        shm_unlink(specifier.c_str());
    }
}