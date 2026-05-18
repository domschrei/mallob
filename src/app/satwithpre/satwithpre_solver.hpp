
#pragma once

#include "app/sat/job/sat_constants.h"
#include "app/satwithpre/preprocessor_orchestrator.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/sys/terminator.hpp"

class SatWithPreSolver {

private:
    const Parameters _params;
    APIConnector& _api;
    const JobDescription& _desc;
    PreprocessorOrchestrator& _po;
    float _time_of_activation;

public:
    SatWithPreSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc),
        _po(*(new PreprocessorOrchestrator(_params, desc, _api))) {}
    ~SatWithPreSolver() {
        if (!_params.terminateAbruptly()) delete &_po;
    }

    JobResult solve() {
        _time_of_activation = Timer::elapsedSeconds();

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = RESULT_UNKNOWN;

        while (res.result == RESULT_UNKNOWN && !isTimeoutHit()) {
            int code = _po.loop();
            if (code == 10) {
                res.setSolution(_po.getModel());
                res.result = RESULT_SAT;
            }
            else if (code == 20) res.result = RESULT_UNSAT;
            else usleep(1000); // 1ms
        }

        LOG(V2_INFO, "SATWP RES ~%i~\n", res.result);
        return res;
    }

private:
    bool isTimeoutHit() const {
        if (_params.timeLimit() > 0 && Timer::elapsedSeconds() >= _params.timeLimit()) {
            LOG(V2_INFO, "SATWP terminate: -T reached\n");
            return true;
        }
        if (_desc.getWallclockLimit() > 0 && (Timer::elapsedSeconds() - _time_of_activation) >= _desc.getWallclockLimit()) {
            LOG(V2_INFO, "SATWP terminate: -jwl reached\n");
            return true;
        }
        if (Terminator::isTerminating()) {
            LOG(V2_INFO, "SATWP terminate: Terminator triggered\n");
            return true;
        }
        return false;
    }
};
