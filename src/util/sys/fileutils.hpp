
#ifndef DOMPASCH_MALLOB_FILE_UTILS_H
#define DOMPASCH_MALLOB_FILE_UTILS_H

#include <string>

class FileUtils {

public:
    static int mkdir(std::string dir);
    static int mergeFiles(std::string globstr, std::string dest, bool removeOriginals);
    static int append(std::string srcFile, std::string destFile);
    static int rm(std::string file);
};

#endif