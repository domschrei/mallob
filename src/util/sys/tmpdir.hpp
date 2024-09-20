
#pragma once

#include "util/sys/fileutils.hpp"
#include <filesystem>
#include <string>
#include <cstdlib>

class TmpDir {

private:
    static std::string _tmpdir;

public:
    static void init(int rank) {
        auto tmpdirFromEnv = std::getenv("MALLOB_TMP_DIR");
        if (tmpdirFromEnv) {
            _tmpdir = std::string(tmpdirFromEnv);
            auto rankSpecificDir = _tmpdir + "/" + std::to_string(rank);
            if (std::filesystem::is_directory(rankSpecificDir)) {
                _tmpdir = rankSpecificDir;
            }
        }
        if (!tmpdirFromEnv || _tmpdir != tmpdirFromEnv)
            setenv("MALLOB_TMP_DIR", _tmpdir.c_str(), 1);
    }
    static std::string getGeneralTmpDir() {
        return _tmpdir;
    }
    static std::string getMachineLocalTmpDir() {
        return "/dev/shm/";
    }
    static void wipe() {
        for (std::string base : {getGeneralTmpDir(), getMachineLocalTmpDir()}) {
            for (std::string file : FileUtils::glob(base + "/edu.kit.iti.mallob.*")) {
                FileUtils::rm(file);
            }
        }
    }
};
