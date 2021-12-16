
#ifndef DOMPASCH_MALLOB_JOB_READER_HPP
#define DOMPASCH_MALLOB_JOB_READER_HPP

#include "data/job_description.hpp"
#include "util/sat_reader.hpp"

namespace JobReader {
    bool read(const std::vector<std::string>& files, SatReader::ContentMode contentMode, JobDescription& desc);
};

#endif
