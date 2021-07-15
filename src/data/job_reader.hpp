
#ifndef DOMPASCH_MALLOB_JOB_READER_HPP
#define DOMPASCH_MALLOB_JOB_READER_HPP

#include "data/job_description.hpp"

namespace JobReader {
    bool read(const std::string& file, JobDescription& desc);
};

#endif
