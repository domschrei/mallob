
#include "shared_memory.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace SharedMemory {

    void* create(const std::string& specifier, size_t size) {

        int memFd = shm_open(specifier.c_str(), O_CREAT | O_RDWR, S_IRWXU);
        if (memFd == -1) {
            perror("Can't open file");
            return nullptr;
        }

        int res = ftruncate(memFd, size);
        if (res == -1) {
            perror("Can't truncate file");
            return nullptr;
        }

        void *buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
        if (buffer == NULL) {
            perror("Can't mmap");
            return nullptr;
        }

        return buffer;
    }

    void* access(const std::string& specifier, size_t size) {

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

        return buffer;
    }

    void free(const std::string& specifier, char* addr, size_t size) {
        munmap(addr, size);
        shm_unlink(specifier.c_str());
    }
}