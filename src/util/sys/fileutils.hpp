
#ifndef DOMPASCH_MALLOB_FILE_UTILS_H
#define DOMPASCH_MALLOB_FILE_UTILS_H

#include <string>

class FileUtils {

public:
    static int mkdir(std::string dir);
    static int mergeFiles(std::string globstr, std::string dest, bool removeOriginals);

};

#endif