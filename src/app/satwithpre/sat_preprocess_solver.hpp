
#pragma once

#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "app/satwithpre/sat_preprocessor.hpp"
#include "comm/mympi.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "mpi.h"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

#include "app/sat/solvers/kissat.hpp"
#include "util/static_store.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include <memory>

class SatPreprocessSolver {

private:
    const Parameters& _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata
    int _cores_allocated {0};

    float _time_of_activation {0};
    float _time_of_retraction_start {0};
    float _time_of_retraction_end {0};
    float _retraction_round_duration;

    bool _base_job_submitted {false};
    volatile bool _base_job_done {false};
    bool _base_job_digested {false};
    nlohmann::json _base_job_submission;
    nlohmann::json _base_job_response;

    bool _prepro_job_submitted {false};
    volatile bool _prepro_job_done {false};
    bool _prepro_job_digested {false};
    nlohmann::json _prepro_job_submission;
    nlohmann::json _prepro_job_response;

    SatPreprocessor& _prepro;

public:
    SatPreprocessSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc), _jobstr("#" + std::to_string(desc.getId())),
        _prepro(*(new SatPreprocessor(desc, _params.preprocessLingeling()))) {}
    ~SatPreprocessSolver() {
        ProcessWideCoreAllocator::get().returnCores(_cores_allocated);
        if (!_params.terminateAbruptly()) delete &_prepro;
    }

    JobResult solve() {
        _time_of_activation = Timer::elapsedSeconds();
        _cores_allocated = ProcessWideCoreAllocator::get().requestCores(1);

        if (_params.preprocessBalancing() >= 0) submitBaseJob();
        _prepro.init();

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = RESULT_UNKNOWN;

        while (!isTimeoutHit()) {
            if (_base_job_done && !_base_job_digested) {
                LOG(V3_VERB, "SATWP base done\n");
                res = jsonToJobResult(_base_job_response, false);
                _base_job_digested = true;
                if (res.result != 0) break;
            }
            if (_prepro_job_done && !_prepro_job_digested) {
                LOG(V3_VERB, "SATWP prepro done\n");
                res = jsonToJobResult(_prepro_job_response, true);
                _prepro_job_digested = true;
                if (res.result != 0) break;
            }
            if (_prepro.done()) {
                // Preprocess solver terminated.
                LOG(V3_VERB, "SATWP preprocessor done\n");
                ProcessWideCoreAllocator::get().returnCores(_cores_allocated);
                _cores_allocated = 0;
                if (_prepro.getResultCode() != 0) {
                    LOG(V3_VERB, "SATWP preprocessor reported result %i\n", _prepro.getResultCode());
                    res.result = _prepro.getResultCode();
                    res.setSolution(std::move(_prepro.getSolution()));
                    break;
                }
            }
            if (_prepro.hasPreprocessedFormula()) {
                LOG(V3_VERB, "SATWP submit preprocessed task\n");
                submitPreprocessedJob(_prepro.extractPreprocessedFormula());
            }
            if (!_base_job_done && _time_of_retraction_end > 0 && Timer::elapsedSeconds() >= _time_of_retraction_end)
                interrupt(_base_job_submission, _base_job_done);
            usleep(3*1000);
        }

        // Terminate sub-jobs
        if (_base_job_submitted && !_base_job_done) {
            interrupt(_base_job_submission, _base_job_done);
            while (!_base_job_done) usleep(5*1000);
        }
        if (_prepro_job_submitted && !_prepro_job_done) {
            interrupt(_prepro_job_submission, _prepro_job_done);
            while (!_prepro_job_done) usleep(5*1000);
        }
        _prepro.interrupt();

        LOG(V2_INFO, "#%i SATWP RES ~%i~\n", _desc.getId(), res.result);
        return res;
    }

private:

    bool isTimeoutHit() const {
        if (_params.timeLimit() > 0 && Timer::elapsedSeconds() >= _params.timeLimit())
            return true;
        if (_desc.getWallclockLimit() > 0 && (Timer::elapsedSeconds() - _time_of_activation) >= _desc.getWallclockLimit())
            return true;
        if (Terminator::isTerminating())
            return true;
        return false;
    }

    void submitBaseJob() {
        auto& json = _base_job_submission;
        json = {
            {"user", "sat-" + std::string(toStr())},
            {"name", std::string(toStr())+":base"},
            {"priority", 1.000},
            {"application", "SAT"}
        };
        if (_params.crossJobCommunication()) json["group-id"] = "1";
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(),
            std::vector<int>(_desc.getFormulaPayload(0), _desc.getFormulaPayload(0)+_desc.getFormulaPayloadSize(0)));
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(_desc.getAppConfiguration().fixedSizeEntryToInt("__NV"));
        json["configuration"]["__NC"] = std::to_string(_desc.getAppConfiguration().fixedSizeEntryToInt("__NC"));
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(_desc.getWallclockLimit() - getAgeSinceActivation()) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(_desc.getCpuLimit() - getAgeSinceActivation()) + "s";

        auto copiedJson = json;
        auto result = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            _base_job_response = std::move(response);
            _base_job_done = true;
        });
        if (result != JsonInterface::Result::ACCEPT) {
            LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
            abort();
        }

        _base_job_submitted = true;
    }

    void submitPreprocessedJob(std::vector<int>&& fPre) {

        assert(fPre.size() > 2);
        int nbClauses = fPre.back(); fPre.pop_back();
        int nbVars = fPre.back(); fPre.pop_back();
        size_t preprocessedSize = fPre.size();

        /*
        // Just for checking whether Kissat actually returns units
        assert(fPre[fPre.size()-1] == 0);
        for (int i = fPre.size()-2; i >= 1; i-=2) {
            if (fPre[i-1]==0 && fPre[i]!=0 && fPre[i+1]==0) {
                // unit clause at the end
                for (int lit : {fPre[i], -1*fPre[i]}) {
                    auto it = std::find(fPre.begin(), fPre.begin()+i, lit);
                    if (it != fPre.begin()+i)
                        LOG(V0_CRIT, "Unit %i (pos %i) occurs in formula (pos %lu)\n", fPre[i], i, it - fPre.begin());
                }
            } else break;
        }
        */

        // begin successively retracting this job
        _time_of_retraction_start = Timer::elapsedSeconds();
        // We want the job to retract over sqrt(p) rounds
        // with a total duration of the job's wallclock time so far.
        float totalRetractionDuration;
        if (_params.preprocessBalancing() == 0) {
            // drop original immediately
            totalRetractionDuration = 0.001;
            _time_of_retraction_end = _time_of_retraction_start;
        } else {
            // replace original gradually, scaled by task age so far and expansion factor
            totalRetractionDuration = std::max(0.001f, getAgeSinceActivation() * _params.preprocessExpansionFactor());
        }
        // If this preprocessing result could be critical in terms of RAM usage,
        // perform the retraction essentially immediately.
        size_t currentSize = _desc.getFormulaPayloadSize(0);
        if (currentSize > 100'000'000 && preprocessedSize/(double)currentSize < 0.75)
            totalRetractionDuration = 0.001;
        _retraction_round_duration = totalRetractionDuration / std::sqrt(MyMpi::size(MPI_COMM_WORLD));
        if (_params.preprocessBalancing() == 1) {
            LOG(V3_VERB, "SATWP %s : Retracting base job over ~%.3fs\n", toStr(), totalRetractionDuration);
            _time_of_retraction_end = _time_of_retraction_start + 1.1f * totalRetractionDuration;
        }

        // Prepare job submission data
        auto& json = _prepro_job_submission;
        json = {
            {"user", "sat-" + std::string(toStr())},
            {"name", std::string(toStr())+":prepro"},
            {"priority", _params.preprocessJobPriority()},
            {"application", "SAT"},
        };
        if (_params.crossJobCommunication()) json["group-id"] = _desc.getGroupId();
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(), fPre);
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(nbVars);
        json["configuration"]["__NC"] = std::to_string(nbClauses);
        if (_params.preprocessBalancing() == 1)
            json["configuration"]["__growprd"] = std::to_string(_retraction_round_duration);
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(_desc.getWallclockLimit() - getAgeSinceActivation()) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(_desc.getCpuLimit() - getAgeSinceActivation()) + "s";

        // Obtain API and submit the job
        auto copiedJson = json;
        auto retcode = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            _prepro_job_response = std::move(response);
            _prepro_job_done = true;
        });
        if (retcode != JsonInterface::ACCEPT) return;

        _prepro_job_submitted = true;
    }

    void interrupt(nlohmann::json& json, volatile bool& doneFlag) {
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
        if (result == JsonInterface::Result::DISCARD) doneFlag = true;
    }

    JobResult jsonToJobResult(nlohmann::json& json, bool convert) {
        LOG(V3_VERB, "SATWP Extract result of %s\n", json["name"].get<std::string>().c_str());
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = json["result"]["resultcode"];
        if (res.result == RESULT_UNKNOWN) return res;
        std::vector<int> solution;
        if (_params.compressModels() && res.result == RESULT_SAT) {
            solution = ModelStringCompressor::decompress(json["result"]["solution"].get<std::string>());
        } else {
            solution = json["result"]["solution"].get<std::vector<int>>();
        }
        if (convert && res.result == RESULT_SAT) {
            LOG(V3_VERB, "SATWP reconstruct original solution\n");
            assert(solution.size() >= 1 && solution[0] == 0);
            _prepro.join(true);
            _prepro.reconstructSolution(solution);
            LOG(V3_VERB, "SATWP original solution reconstructed\n");
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
