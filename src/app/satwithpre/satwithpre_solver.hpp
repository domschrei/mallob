
#pragma once

#include "app/app_terminate_checker.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/satwithpre/preprocessor_orchestrator.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"

class SatWithPreSolver {

private:
    const Parameters _params;
    APIConnector& _api;
    const JobDescription& _desc;
    std::unique_ptr<PreprocessorOrchestrator> _po;
    AppTerminateChecker _term;

public:
    SatWithPreSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc),
        _po(new PreprocessorOrchestrator(_params, _desc, _api)),
        _term(_params, _desc) {}
    ~SatWithPreSolver() {
        if (_params.terminateAbruptly()) _po.release();
    }

    JobResult solve() {
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = RESULT_UNKNOWN;

        while (res.result == RESULT_UNKNOWN && !_term.isTimeoutHit()) {
            int code = _po->loop();
            if (code == 10) {
                res.setSolution(_po->getModel());
                res.result = RESULT_SAT;
            }
            else if (code == 20) res.result = RESULT_UNSAT;
            else if (code == -1) {
                LOG(V2_INFO, "SATWP terminate: no actors left\n");
                break;
            }
            usleep(1000); // no result yet, sleep for 1ms
        }
        _po->stopAll();
        _po->finalizeProofs();

        LOG(V2_INFO, "SATWP RES ~%i~\n", res.result);
        return res;
    }
};
