
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <string>
#include <vector>
#include <memory>

class SatReader {

private:
    std::string _filename;

public:
    SatReader(std::string filename) : _filename(filename) {}
    std::shared_ptr<std::vector<int>> read();
};

#endif