
#pragma once

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
    static std::string get() {
        return _tmpdir;
    }
};
