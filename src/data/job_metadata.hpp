
#ifndef DOMPASCH_MALLOB_JOB_METADATA_HPP
#define DOMPASCH_MALLOB_JOB_METADATA_HPP

#include <string>
#include <vector>
#include <memory>

#include "data/job_description.hpp"
#include "util/sat_reader.hpp"

struct JobMetadata {
    std::shared_ptr<JobDescription> description;
    std::vector<std::string> files;
    SatReader::ContentMode contentMode;

    std::vector<int> dependencies;
    bool done = false;
    bool interrupt = false;

    bool operator==(const JobMetadata& other) const {
        if (!description != !(other.description)) return false;
        if (!description) return files == other.files;
        if (description->getId() != other.description->getId()) return false;
        if (description->getRevision() != other.description->getRevision()) return false;
        return true;
    }
    bool operator!=(const JobMetadata& other) const {
        return !(*this == other);
    }

    std::string getFilesList() const {
        std::string list = "{";
        for (auto& file : files) list += file + ",";
        return list.substr(0, list.size()-(files.empty()?0:1)) + "}";
    }
};

#endif
