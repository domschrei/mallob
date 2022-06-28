
#ifndef DOMPASCH_MALLOB_JOB_METADATA_HPP
#define DOMPASCH_MALLOB_JOB_METADATA_HPP

#include <string>
#include <vector>
#include <memory>

#include "data/job_description.hpp"

struct JobMetadata {

    std::unique_ptr<JobDescription> description;
    std::vector<std::string> files;
    std::vector<int> dependencies;
    bool done = false;
    bool interrupt = false;

    JobMetadata() {}
    JobMetadata(JobMetadata&& other) : 
        description(std::move(other.description)), 
        files(std::move(other.files)), 
        dependencies(std::move(other.dependencies)),
        done(other.done), interrupt(other.interrupt) {}
    
    JobMetadata& operator=(JobMetadata&& other) {
        *this = JobMetadata(std::move(other));
        return *this;
    }

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

    bool hasFiles() const {
        return !files.empty();
    }

    std::string getFilesList() const {
        std::string list = "{";
        for (auto& file : files) list += file + ",";
        return list.substr(0, list.size()-(files.empty()?0:1)) + "}";
    }
};

#endif
