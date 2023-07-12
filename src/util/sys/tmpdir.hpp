
#pragma once

#include <string>
#include <cstdlib>

class TmpDir {

public:
    static std::string get() {
        auto tmpdir = std::getenv("MALLOB_TMP_DIR");
        if (tmpdir) {
            return std::string(tmpdir);
        }
        return "/tmp";
    }
};
