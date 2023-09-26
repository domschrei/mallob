
#pragma once

#include <string>

#include "util/sys/fileutils.hpp"
#include "util/logger.hpp"

class RankSpecificFileFetcher {

private:
    int _internal_rank;
    bool _has_rank_specific_template {false};

public:
    RankSpecificFileFetcher(int internalRank) : _internal_rank(internalRank) {}

    std::string get(const std::string& path) {
        std::string templateFile = path;
        // Check if a client-specific template file is present
        std::string clientSpecificFile = templateFile + "." + std::to_string(_internal_rank);
        if (FileUtils::isRegularFile(clientSpecificFile)) {
            _has_rank_specific_template = true;
            return clientSpecificFile;
        }
        // Is template file itself valid?
        if (!FileUtils::isRegularFile(templateFile)) {
            LOG(V1_WARN, "Template file %s does not exist\n", templateFile.c_str());        
            return "";
        }
        return templateFile;
    }
    
    bool hasRankSpecificTemplate() const {
        return _has_rank_specific_template;
    }
};