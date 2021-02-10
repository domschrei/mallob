
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <string>
#include <vector>
#include <memory>

#include "data/job_description.hpp"

class SatReader {

private:
    std::string _filename;

public:
    SatReader(std::string filename) : _filename(filename) {}
    bool read(JobDescription& desc);
};

#endif