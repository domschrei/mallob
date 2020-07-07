
#include "shared_memory.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

namespace SharedMemory {

    void* create(const std::string& specifier, size_t size) {

        int memFd = shm_open(specifier.c_str(), O_CREAT | O_RDWR, S_IRWXU);
        assert(memFd != -1);

        int res = ftruncate(memFd, size);
        assert(res != -1);

        void *buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
        assert(buffer != nullptr);
        close(memFd);

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
        close(memFd);

        return buffer;
    }

    void free(const std::string& specifier, char* addr, size_t size) {
        munmap(addr, size);
        shm_unlink(specifier.c_str());
    }
}