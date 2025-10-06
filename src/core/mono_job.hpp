
#pragma once

#include <string>

#include "interface/api/api_registry.hpp"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

class MonoJob {

private:
    Parameters& _params;
    std::string _job_str;
    int _revision {0};
    bool _done = false;
    float _time_of_start;

public:
    MonoJob(Parameters& params) : _params(params) {}
    void submitFirst() {
        // Write a job JSON for the singular job to solve
        nlohmann::json json = {
            {"user", "admin"},
            {"name", "mono-job-" + std::to_string(_revision)},
            {"files", {_params.monoFilename()}},
            {"description-id", "mono-job-desc-0"},
            {"priority", 1.000},
            {"application", getMonoApplicationName()},
            {"incremental", _params.monoIncrements() > 0}
        };
        _job_str = "admin.mono-job-" + std::to_string(_revision);
        if (_params.crossJobCommunication()) json["group-id"] = "1";
        if (_params.jobWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(_params.jobWallclockLimit()) + "s";
        if (_params.jobCpuLimit() > 0) {
            json["cpu-limit"] = std::to_string(_params.jobCpuLimit()) + "s";
        }
        _time_of_start = Timer::elapsedSeconds();
        submit(json);
    }
    bool done() const {
        return _done;
    }

private:
    std::string getMonoApplicationName() {
        // Parse application name
        auto app = _params.monoApplication();
        std::transform(app.begin(), app.end(), app.begin(), ::toupper);
        LOG(V2_INFO, "Assuming application \"%s\" for mono job\n", app.c_str());
        return app;
    }

    void submit(nlohmann::json& json) {
        LOG(V2_INFO, "Submit mono job (increment) %s\n", json.dump().c_str());
        _revision++;
        auto result = APIRegistry::get().submit(json, 
            [&](nlohmann::json &resp) {monoResponseCallback(resp);});
        if (result == JsonInterface::Result::DISCARD) {
            LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
            abort();
        };
        if (result == JsonInterface::Result::ACCEPT_CONCLUDE) {
            LOG(V2_INFO, "Mono job done\n");
            _done = true;
            return;
        }
    }

    void monoResponseCallback(nlohmann::json& response) {
        if (_params.monoIncrements() == 0 || _revision > _params.monoIncrements()) {
            LOG(V2_INFO, "Mono job done\n");
            _done = true;
            return;
        }
        nlohmann::json nextJson = {
            {"user", response["user"]},
            {"name", "mono-job-" + std::to_string(_revision)},
            {"files", {_params.monoFilename()}},
            {"description-id", "mono-job-desc-" + std::to_string(_revision)},
            {"priority", 1.000},
            {"application", getMonoApplicationName()},
            {"incremental", true},
            {"precursor", _job_str},
            {"done", _revision == _params.monoIncrements() || response["result"]["resultcode"] == 0},
        };
        if (_params.jobWallclockLimit() > 0)
            nextJson["wallclock-limit"] = std::to_string(std::max(0.0001f,
                _params.jobWallclockLimit() - (Timer::elapsedSeconds() - _time_of_start)
            )) + "s";
        _job_str = "admin.mono-job-" + std::to_string(_revision);
        submit(nextJson);
    }
};
