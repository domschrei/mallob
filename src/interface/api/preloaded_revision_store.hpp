
#pragma once

#include "data/app_configuration.hpp"
#include "util/tsl/robin_map.h"
#include "util/sys/threading.hpp"

class PreloadedRevisionStore {

private:
    Mutex _mtx_revisions;
    tsl::robin_map<std::string, std::vector<std::vector<int>>> _revisions_per_user_job;

public:
    void storePayload(const std::string& key, int rev, std::vector<int>&& payload) {
        auto lock = _mtx_revisions.getLock();
        auto& revs = _revisions_per_user_job[key];
        if (rev >= revs.size()) revs.resize(rev+1);
        revs[rev] = std::move(payload);
    }
    std::vector<int> withdrawPayload(const std::string& key, int rev) {
        auto lock = _mtx_revisions.getLock();
        auto payload = std::move(_revisions_per_user_job[key][rev]);
        return payload;
    }
};
