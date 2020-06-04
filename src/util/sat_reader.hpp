
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <string>
#include <vector>
#include <memory>

class SatReader {

private:
    std::string _filename;
    int _num_vars = -1;

public:
    SatReader(std::string filename) : _filename(filename) {}
    std::shared_ptr<std::vector<int>> read();
    int getNumVars() {return _num_vars;}
};

#endif