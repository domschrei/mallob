
#pragma once

#include <vector>

#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/static_store.hpp"
#include "util/sys/timer.hpp"
#include <sys/stat.h>
#include <unistd.h>

class MallobSweepPreprocessActor : public SatPreprocessActor {

private:
    const JobDescription& _desc; // contains our instance to solve and all metadata
    APIConnector& _api; // for submitting jobs to Mallob
    const int _job_id;
    const float _time_of_activation;

    nlohmann::json _base_json;

public:
    MallobSweepPreprocessActor(const Parameters& params, const JobDescription& desc, const std::string& name,
            APIConnector& api, std::vector<int>&& formula, float timeOfActivation) :
        SatPreprocessActor(params, name, std::move(formula)), _desc(desc), _api(api),
            _job_id(desc.getId()), _time_of_activation(timeOfActivation) {

        static int _actor_counter = 1;

        _jobstr = "#" + std::to_string(_job_id) + ":mal:" + std::to_string(_actor_counter++);
    }
    ~MallobSweepPreprocessActor() {}

    void preprocessAsync() override {
        submitJob();
    }
    // Nothing to do
    void reconstructSolution(std::vector<int>& sol) override {}

    void interrupt() override {
        interrupt(_base_json);
    }

private:
    void submitJob() {
        // Prepare job submission data
        auto& json = _base_json;
        json = {
            {"user", "sweep-" + std::string(toStr())},
            {"name", std::string(toStr())+":SWEEP:job"},
            {"priority", _params.preprocessSweepPriority()},
            {"application", "SWEEP"},
        };
        // if (_params.crossJobCommunication()) json["group-id"] = "1";
        if (_params.crossJobCommunication()) json["group-id"] = std::to_string(_desc.getGroupId());
        LOG(V0_CRIT, "MallobSweep GroupId %i \n", _desc.getGroupId());


        auto f = std::vector<int>(_input_cnf.begin(), _input_cnf.end() - 2);
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(), std::move(f));
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(nbInputVars());
        json["configuration"]["__NC"] = std::to_string(nbInputClauses());
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(
                std::max(0.001f, _desc.getWallclockLimit() - getAgeSinceActivation())) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(
            std::max(0.001f, _desc.getCpuLimit() - getAgeSinceActivation())) + "s";

        applySuccessiveGrowth(json);

        auto copiedJson = json;
        auto result = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            auto res = jsonToJobResult(response);
            if (res.result == RESULT_SAT) _result = SAT;
            else if (res.result == RESULT_UNSAT) _result = UNSAT;
            else if (res.result == RESULT_SIMPLIFIED) _result = SIMPLIFIED;
            else _result = NONE;
        });
        if (result != JsonInterface::Result::ACCEPT) {
            LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
            abort();
        }
    }

    void applySuccessiveGrowth(nlohmann::json& json) {
        // begin successively retracting this job
        float _time_of_retraction_start = Timer::elapsedSeconds();
        // We want the job to retract over sqrt(p) rounds
        // with a total duration of the job's wallclock time so far.
        float totalRetractionDuration;
        if (_params.preprocessBalancing() == 0 || MyMpi::size(MPI_COMM_WORLD) == 1) {
            // drop original immediately
            totalRetractionDuration = 0.001;
        } else {
            // replace original gradually, scaled by task age so far and expansion factor
            totalRetractionDuration = std::max(0.001f, getAgeSinceActivation() * _params.preprocessExpansionFactor());
        }
        // If this preprocessing result could be critical in terms of RAM usage,
        // perform the retraction essentially immediately.
        size_t currentSize = _desc.getFormulaPayloadSize(0);
        if (currentSize > 100'000'000 /*&& preprocessedSize/(double)currentSize < 0.75*/)
            totalRetractionDuration = 0.001;
        double _retraction_round_duration = totalRetractionDuration / std::sqrt(MyMpi::size(MPI_COMM_WORLD));
        if (_params.preprocessBalancing() == 1 && MyMpi::size(MPI_COMM_WORLD) > 1) {
            LOG(V3_VERB, "SATWP %s : Retracting base job over ~%.3fs\n", toStr(), totalRetractionDuration);
        }
        if (_params.preprocessBalancing() == 1 && MyMpi::size(MPI_COMM_WORLD) > 1)
            json["configuration"]["__growprd"] = std::to_string(_retraction_round_duration);
    }

    void interrupt(nlohmann::json& json/*, volatile bool& doneFlag*/) {
        if (!json.count("name")) return;
        LOG(V3_VERB, "SATWP Interrupt %s\n", json["name"].get<std::string>().c_str());
        nlohmann::json jsonInterrupt {
            {"name", json["name"]},
            {"user", json["user"]},
            {"application", json["application"]},
            {"incremental", false},
            {"interrupt", true}
        };
        // In this particular case, the callback is never called.
        // Instead, the callback of the job's original submission is called.
        auto result = _api.submit(jsonInterrupt, [&](nlohmann::json& result) {assert(false);});
        //if (result == JsonInterface::Result::DISCARD) doneFlag = true;
    }

    JobResult jsonToJobResult(nlohmann::json& json) {
        LOG(V3_VERB, "SATWP Extract result of %s\n", json["name"].get<std::string>().c_str());
        JobResult res;
        res.id = _job_id;
        res.revision = 0;
        res.result = json["result"]["resultcode"];
        if (res.result == RESULT_UNKNOWN) return res;
        std::vector<int> solution;
        if (_params.compressModels() && res.result == RESULT_SAT) {
            solution = ModelStringCompressor::decompress(json["result"]["solution"].get<std::string>());
        } else {
            solution = json["result"]["solution"].get<std::vector<int>>();
        }
        if (res.result == RESULT_SAT) {
            assert(solution.size() >= 1 && solution[0] == 0);
            _model = std::move(solution);
        } else if (res.result == RESULT_SIMPLIFIED) {
            _output_cnf = std::move(solution);
            //already contains metadata #vals and #clauses in the last two entries
        }
        res.setSolution(std::move(solution));
        LOG(V3_VERB, "SATWP %s extracted\n", json["name"].get<std::string>().c_str());
        return res;
    }

    float getAgeSinceActivation() const {
        return Timer::elapsedSeconds() - _time_of_activation;
    }
    std::string _jobstr;
    const char* toStr() const {
        return _jobstr.c_str();
    }
};

