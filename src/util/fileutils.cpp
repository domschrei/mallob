
#include "fileutils.h"

#include <cstdlib>
#include <glob.h>
#include <vector>

int FileUtils::mkdir(std::string dir) {
    return system(("mkdir -p \"" + dir + "\"").c_str());
}

int errfunc(const char* epath, int eerrno) {
    // TODO handle
}

int FileUtils::mergeFiles(std::string globstr, std::string dest, bool removeOriginals) {
    
    glob_t result;
    int status = glob(globstr.c_str(), GLOB_NOSORT, errfunc, &result);
    if (status == 0) {
        std::string files = "";
        for (int i = 0; i < result.gl_pathc; i++) {
            files += std::string(result.gl_pathv[i]) + " ";
        }
        status = files.size() == 0;
        if (status == 0) {
            std::string cmd = "cat " + files + " >> " + dest;
            status = system(cmd.c_str());
            if (status == 0 && removeOriginals) {
                cmd = "rm " + files;
                status = system(cmd.c_str());
            }
        }
    }
    
    globfree(&result);
    return status;
}