
#ifndef DOMPASCH_MALLOB_JOB_METADATA_HPP
#define DOMPASCH_MALLOB_JOB_METADATA_HPP

#include <string>
#include <vector>
#include <memory>

#include "data/job_description.hpp"
#include "util/sat_reader.hpp"

struct JobMetadata {
    std::shared_ptr<JobDescription> description;
    std::string file;
    SatReader::ContentMode contentMode;

    std::vector<int> dependencies;
    bool done = false;
    bool interrupt = false;

    bool operator==(const JobMetadata& other) const {
        if (!description != !(other.description)) return false;
        if (!description) return file == other.file;
        if (description->getId() != other.description->getId()) return false;
        if (description->getRevision() != other.description->getRevision()) return false;
        return true;
    }
    bool operator!=(const JobMetadata& other) const {
        return !(*this == other);
    }
};

#endif
