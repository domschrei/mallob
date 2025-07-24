
#pragma once

#include <filesystem>
#include <string>
#include <cstdlib>

#include "util/sys/fileutils.hpp"
#include "util/assert.hpp"

class TmpDir {

private:
    static std::string _tmpdir;

public:
    static void init(int rank, const std::string& generalTmpDir) {
        _tmpdir = generalTmpDir;
        auto rankSpecificDir = _tmpdir + "/" + std::to_string(rank);
        if (std::filesystem::is_directory(rankSpecificDir)) {
            _tmpdir = rankSpecificDir;
        }
    }
    // Get this Mallob run's general "tmp" directory, mostly for user-facing temporary information
    // (logging, profiling, interfaces, ...) which can be set by the user via the -tmp program option.
    // Calls to this method must be preceded by an initializing call to TmpDir::init(rank, generalTmpDir).
    static std::string getGeneralTmpDir() {
        assert(!_tmpdir.empty());
        return _tmpdir;
    }
    // Get a directory which is guaranteed to be purely local (i.e., restricted to your own
    // physical machine) on all reasonable systems. It may reside in RAM, so it shouldn't be
    // used to write indefinite amounts of data (e.g., logs). It should be rather fast and can be used
    // for internal temporary information (inter-process communication, local peer discovery, etc.).
    static std::string getMachineLocalTmpDir() {
        return "/dev/shm/";
    }
    // Wipe both the general tmp directory and the machine-local tmp directory.
    // Use with caution and only before or after the actual Mallob run.
    static void wipe() {
        for (std::string base : {getGeneralTmpDir(), getMachineLocalTmpDir()}) {
            for (std::string file : FileUtils::glob(base + "/edu.kit.iti.mallob.*")) {
                FileUtils::rm(file);
            }
        }
    }
};
