
#include "fileutils.hpp"

#include <cstdlib>
#include <glob.h>
#include <stdio.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <sys/stat.h>

#include "util/logger.hpp"

bool FileUtils::isRegularFile(const std::string& file) {
    struct stat sb;
    return stat(file.c_str(), &sb) == 0 && S_ISREG(sb.st_mode);
}

bool FileUtils::isDirectory(const std::string& dirpath) {
    struct stat sb;
    return stat(dirpath.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

int FileUtils::mkdir(const std::string& dir) {
    for (size_t i = 0; i < dir.size(); i++) {
        if (dir[i] == '/' && i > 0 && i+1 < dir.size()) {
            std::string subdir(dir.begin(), dir.begin() + i);
            int res = ::mkdir(subdir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);   
            if (res != 0 && errno != EEXIST) {
                LOG(V0_CRIT, "[ERROR] mkdir -p \"%s\" failed, errno %i\n", subdir.c_str(), errno);
                return res;
            }  
        }
    }
    auto res = ::mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
    if (res == 0 || errno == EEXIST) return 0;
    LOG(V0_CRIT, "[ERROR] mkdir -p \"%s\" failed, errno %i\n", dir.c_str(), errno);
    return res;
}

int errfunc(const char* epath, int eerrno) {
    // TODO handle
    return 0;
}

int FileUtils::mergeFiles(const std::string& globstr, const std::string& dest, bool removeOriginals) {
    
    // For each file matched
    for (const auto& file : glob(globstr)) {
        int status = append(file, dest);
        if (status == 0 && removeOriginals) {
            rm(file);
        }
    }
    return 0;
}

int FileUtils::append(const std::string& srcFile, const std::string& destFile) {
    
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

int FileUtils::rm(const std::string& file) {
    return remove(file.c_str());
}

std::vector<std::string> FileUtils::glob(const std::string& pattern) {
    std::vector<std::string> files;

    glob_t result;
    int status = ::glob(pattern.c_str(), /*flags=*/0, errfunc, &result);
    
    if (status == GLOB_NOMATCH) {
        // This is not an error: The set of files to merge is merely empty.
        globfree(&result);
        return files;
    }
    if (status == GLOB_ABORTED || status == GLOB_NOSPACE) {
        globfree(&result);
        return files;
    }

    // For each file matched
    for (size_t i = 0; i < result.gl_pathc; i++) {
        std::string file = std::string(result.gl_pathv[i]);
        files.push_back(std::move(file));
    }
    
    globfree(&result);
    return files;
}
