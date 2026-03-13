
#pragma once

#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"
#include <string>
class ImpCheckProgramLookup {

public:
    static std::string getParserExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "parse");
    }
    static std::string getCheckerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "check");
    }
    static std::string getConfirmerExecutablePath(bool incremental) {
        return getExecutablePath(incremental, "confirm");
    }

private:
    static std::string getExecutablePath(bool incremental, const std::string& role) {
        const std::string base = std::string(MALLOB_SUBPROC_DISPATCH_PATH);

        std::string executable = incremental ? "iimpcheck_" + role : "impcake_" + role;
        if (FileUtils::isRegularFile(base + executable))
            return executable;

        if (!FileUtils::isRegularFile(base + "impcheck_" + role)) {
            // ERROR
            LOG(V0_CRIT, "[ERROR] Did not find suitable ImpCheck executable!\n");
            abort();
        }
        return "impcheck_" + role;
    }
};
