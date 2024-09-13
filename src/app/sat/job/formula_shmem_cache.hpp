
#pragma once

#include "robin_map.h"
#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/threading.hpp"
#include <cstring>
#include <string>
#include <unistd.h>

class FormulaSharedMemoryCache {

private:
    struct Entry {
        std::string shmemPath;
        const int* data;
        size_t size;
        std::vector<std::string> lockFilePaths;
    };

public:
    void* createOrAccess(int descriptionId, const std::string& userLabel, size_t size, const void* data, std::string& outShmemId, bool& outCreated) {
        LOG(V5_DEBG, "CACHE create or access %i via %s\n", descriptionId, userLabel.c_str());
        outCreated = false;

        // Acquire RAII lock to manipulate this job description's shared memory exclusively
        FileBasedLock lock("jobdesc", descriptionId);

        // Add a reference to the shared memory segment
        bool ok = FileUtils::create(getRefFilename(descriptionId, userLabel));
        assert(ok);

        // Try to create the shared memory segment
        const std::string shmemId = getShmemId(descriptionId);
        void* shmem = SharedMemory::create(shmemId, size);
        bool mustWaitUntilInitialized = false;
        if (shmem) {
            // successfully created - initialize while releasing the general lock.
            // Instead, we acquire a file specifically representing the initialization process.
            outCreated = true;
            FileBasedLock createLock("jobdesccreate", descriptionId);
            lock.unlock();
            memcpy(shmem, data, size);
            LOG(V5_DEBG, "CACHE created %i\n", descriptionId);
        } else {
            // shmem file already exists - access it
            mustWaitUntilInitialized = true;
            shmem = SharedMemory::access(shmemId, size);
            LOG(V5_DEBG, "CACHE accessed %i\n", descriptionId);
        }

        lock.unlock();

        if (mustWaitUntilInitialized) {
            while (FileUtils::exists(FileBasedLock::opLockFile("jobdesccreate", descriptionId)))
                usleep(3000);
        }

        // Return the shared memory pointer and its shared memory ID
        outShmemId = shmemId;
        return shmem;
    }

    void* tryAccess(int descriptionId, const std::string& userLabel, size_t size, std::string& outShmemId) {
        LOG(V5_DEBG, "CACHE try preregister %i via %s\n", descriptionId, userLabel.c_str());

        // Acquire RAII lock to manipulate this job description's shared memory exclusively
        FileBasedLock lock("jobdesc", descriptionId);

        // Try to access the shared memory segment
        void* shmem;
        const std::string shmemId = getShmemId(descriptionId);
        if (FileUtils::exists("/dev/shm" + shmemId)) {
            // Shared memory segment seems to be present.
            shmem = SharedMemory::access(shmemId, size);
            LOG(V5_DEBG, "CACHE accessed %i\n", descriptionId);

            // Add a reference to the shared memory segment
            bool ok = FileUtils::create(getRefFilename(descriptionId, userLabel));
            assert(ok);
            outShmemId = shmemId;
            return shmem;
        }
        
        return nullptr;
    }

    void drop(int descriptionId, const std::string& userLabel, size_t size, void* data) {
        LOG(V5_DEBG, "CACHE drop %i via %s\n", descriptionId, userLabel.c_str());

        // Acquire RAII lock to manipulate this job description's shared memory
        FileBasedLock lock("jobdesc", descriptionId);

        // Remove your own lock file
        int res = FileUtils::rmf(getRefFilename(descriptionId, userLabel));
        assert(res == 0);

        auto lockFiles = FileUtils::glob(getRefFilename(descriptionId, "*"));
        LOG(V5_DEBG, "CACHE dropped %i via %s\n", descriptionId, userLabel.c_str());
        if (lockFiles.empty()) {
            // reference count became zero: delete
            LOG(V5_DEBG, "CACHE delete %i\n", descriptionId);
            // FIXME leads to crashes if enabled.
            //SharedMemory::free(getShmemId(descriptionId), (char*) data, size);
            FileUtils::rmf("/dev/shm" + getShmemId(descriptionId));
        }
    }

    static std::string getShmemId(int descriptionId) {
        return "/edu.kit.iti.mallob.jobdesc." + std::to_string(descriptionId);
    }

private:
    struct FileBasedLock {
        std::string label;
        int id;
        bool locked {false};
        FileBasedLock(const std::string& label, int id, bool lockImmediately = true) : label(label), id(id) {
            if (lockImmediately) lock();
        }
        void lock() {
            if (label.empty() || locked) return;
            while (!FileUtils::createExclusively(opLockFile(label, id)))
                usleep(3000);
            locked = true;
        }
        bool tryLock() {
            if (label.empty()) return false;
            if (locked) return true;
            if (!FileUtils::createExclusively(opLockFile(label, id))) return false;
            locked = true;
            return true;
        }
        void unlock() {
            if (label.empty() || !locked) return;
            int res = FileUtils::rmf(opLockFile(label, id));
            assert(res == 0);
            locked = false;
        }
        ~FileBasedLock() {
            unlock();
        }
        static std::string opLockFile(const std::string& label, int id) {
            return "/dev/shm/edu.kit.iti.mallob." + label + "-lock." + std::to_string(id);
        }
    };

    std::string getRefFilename(int descriptionId, const std::string& userLabel) const {
        return "/dev/shm/" + userLabel + ".jobdesc-ref." + std::to_string(descriptionId);
    }
};

class StaticFormulaSharedMemoryCache {
private:
    static FormulaSharedMemoryCache singleton;
public:
    static FormulaSharedMemoryCache& get() {return singleton;}
};
