
#pragma once

#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "util/params.hpp"

class CncController {

private:
    const Parameters& _params;
    JobDescription& _desc;

public:
    CncController(const Parameters& params, JobDescription& desc) : _params(params), _desc(desc) {}

    JobResult solve() {
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 0; // unknown
        return res;
    }
};
