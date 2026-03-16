
#pragma once

#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"
#include <string>
class ImpCheckProgramLookup {

public:
    static std::string getParserExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "parse", true);
    }
    static std::string getCheckerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "check", true);
    }
    static std::string getConfirmerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "confirm", true);
    }

    static std::string tryGetParserExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "parse", false);
    }
    static std::string tryGetCheckerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "check", false);
    }
    static std::string tryGetConfirmerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "confirm", false);
    }

private:
    static std::string getExecutablePath(bool incremental, const std::string& role, bool abortAtFailure) {
        const std::string base = std::string(MALLOB_SUBPROC_DISPATCH_PATH);

        std::string executable = incremental ? "iimpcheck_" + role : "impcake_" + role;
        if (FileUtils::isRegularFile(base + executable))
            return executable;

        if (!FileUtils::isRegularFile(base + "impcheck_" + role)) {
            if (abortAtFailure) {
                // ERROR
                LOG(V0_CRIT, "[ERROR] Did not find suitable ImpCheck executable!\n");
                abort();
            } else return "";
        }
        return "impcheck_" + role;
    }
};
