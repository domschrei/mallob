
#pragma once

#include "app/sat/parse/sat_reader.hpp"
#include "data/job_description.hpp"
#include "util/params.hpp"

class MaxSatReader {

private:
    SatReader _sat_reader;

public:
    MaxSatReader(const Parameters& params, const std::string& inputPath) :
        _sat_reader(params, inputPath) {

    }
    bool read(JobDescription& desc) {
        // TODO Generalize the reader to 
        return _sat_reader.read(desc);
    }
};
