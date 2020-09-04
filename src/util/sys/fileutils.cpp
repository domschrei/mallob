
#include "fileutils.hpp"

#include <cstdlib>
#include <glob.h>
#include <stdio.h>
#include <vector>
#include <iostream>
#include <fstream>

int FileUtils::mkdir(std::string dir) {
    return system(("mkdir -p \"" + dir + "\"").c_str());
}

int errfunc(const char* epath, int eerrno) {
    // TODO handle
    return 0;
}

int FileUtils::mergeFiles(std::string globstr, std::string dest, bool removeOriginals) {
    
    glob_t result;
    int status = glob(globstr.c_str(), /*flags=*/0, errfunc, &result);
    
    if (status == GLOB_NOMATCH) {
        // This is not an error: The set of files to merge is merely empty.
        globfree(&result);
        return 0;
    }
    if (status == GLOB_ABORTED) {
        globfree(&result);
        return 1;
    }
    if (status == GLOB_NOSPACE) {
        globfree(&result);
        return 2;
    }

    // For each file matched
    for (size_t i = 0; i < result.gl_pathc; i++) {
        std::string file = std::string(result.gl_pathv[i]);
        status = append(file, dest);
        if (status != 0) {
            globfree(&result);
            return 2 + status;
        } 
        if (removeOriginals) {
            status = rm(file);
            if (status != 0) {
                globfree(&result);
                return -2 - status;
            }
        }
    }
    
    return status;
}

int FileUtils::append(std::string srcFile, std::string destFile) {
    
    std::ifstream src(srcFile);
    std::ofstream dest(destFile, std::ios::app);

    if (!src.is_open()) {
        return 1;
    } else if (!dest.is_open()) {
        return 2;
    } else {
        dest << src.rdbuf();
        return 0;
    }
}

int FileUtils::rm(std::string file) {
    return remove(file.c_str());
}