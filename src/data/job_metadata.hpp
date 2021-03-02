
#ifndef DOMPASCH_MALLOB_JOB_METADATA_HPP
#define DOMPASCH_MALLOB_JOB_METADATA_HPP

#include <string>
#include <vector>
#include <memory>

#include "data/job_description.hpp"

struct JobMetadata {
    std::shared_ptr<JobDescription> description;
    std::string file;
    std::vector<int> dependencies;

    bool operator==(const JobMetadata& other) const {
        return file == other.file;
    }
    bool operator!=(const JobMetadata& other) const {
        return !(*this == other);
    }
};

#endif
