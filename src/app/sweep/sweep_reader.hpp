
#ifndef DOMPASCH_MALLOB_SWEEP_READER_HPP
#define DOMPASCH_MALLOB_SWEEP_READER_HPP

#include <string>
#include <vector>

#include "data/job_description.hpp"

class JobDescription;

namespace SweepReader {
    /*
    Read a revision of a job of the sweep application.
    */
    bool read(const std::vector<std::string>& filenames, JobDescription& desc);
};

#endif
