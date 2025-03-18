
#pragma once

#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "comm/mympi.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "mpi.h"
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

    float _time_of_activation {0};
    float _time_of_retraction_start {0};
    float _time_of_retraction_end {0};
    float _retraction_round_duration;

    volatile bool _base_job_done {false};
    bool _base_job_digested {false};
    nlohmann::json _base_job_submission;
    nlohmann::json _base_job_response;

    std::unique_ptr<PortfolioSolverInterface> _solver;
    std::future<void> _fut_solver;
    int _solver_result {0};
    volatile bool _solver_done {false};

    bool _prepro_job_submitted {false};
    volatile bool _prepro_job_done {false};
    bool _prepro_job_digested {false};
    nlohmann::json _prepro_job_submission;
    nlohmann::json _prepro_job_response;

public:
    SatPreprocessSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc), _jobstr("#" + std::to_string(desc.getId())) {}
    ~SatPreprocessSolver() {
        LOG(V3_VERB, "SATWP enter destructor\n");
        if (_fut_solver.valid()) _fut_solver.get();
        _solver->cleanUp();
        LOG(V3_VERB, "SATWP leave destructor\n");
    }

    JobResult solve() {
        _time_of_activation = Timer::elapsedSeconds();

        submitBaseJob();
        launchPreprocessor();

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = RESULT_UNKNOWN;

        while (!isTimeoutHit()) {
            if (_base_job_done && !_base_job_digested) {
                LOG(V3_VERB, "SATWP base done\n");
                res = jsonToJobResult(_base_job_response);
                _base_job_digested = true;
                if (res.result != 0) break;
            }
            if (_prepro_job_done && !_prepro_job_digested) {
                LOG(V3_VERB, "SATWP prepro done\n");
                res = jsonToJobResult(_prepro_job_response);
                _prepro_job_digested = true;
                if (res.result != 0) break;
            }
            if (_solver_done) {
                // Preprocess solver terminated.
                LOG(V3_VERB, "SATWP preprocessor done\n");
                if (_solver_result != 0) {
                    LOG(V3_VERB, "SATWP preprocessor reported result %i\n", _solver_result);
                    res.result = _solver_result;
                    res.setSolution(_solver->getSolution());
                    break;
                }
                _solver_done = false;
            }
            if (_solver->hasPreprocessedFormula()) {
                LOG(V3_VERB, "SATWP submit preprocessed task\n");
                submitPreprocessedJob(std::move(_solver->extractPreprocessedFormula()));
            }
            if (!_base_job_done && _time_of_retraction_end > 0 && Timer::elapsedSeconds() >= _time_of_retraction_end)
                interrupt(_base_job_submission, _base_job_done);
            usleep(3*1000);
        }

        // Terminate sub-jobs
        if (!_base_job_done) {
            interrupt(_base_job_submission, _base_job_done);
            while (!_base_job_done) usleep(5*1000);
        }
        if (_prepro_job_submitted && !_prepro_job_done) {
            interrupt(_prepro_job_submission, _prepro_job_done);
            while (!_prepro_job_done) usleep(5*1000);
        }
        _solver->interrupt();

        LOG(V3_VERB, "SATWP returning result %i\n", res.result);
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
    }

    void submitPreprocessedJob(std::vector<int>&& fPre) {

        assert(fPre.size() > 2);
        int nbClauses = fPre.back(); fPre.pop_back();
        int nbVars = fPre.back(); fPre.pop_back();
        size_t preprocessedSize = fPre.size();

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
            totalRetractionDuration = getAgeSinceActivation() * _params.preprocessExpansionFactor();
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

    void launchPreprocessor() {
        SolverSetup setup;
        setup.logger = &Logger::getMainInstance();
        setup.solverType = 'p';
        _solver.reset(new Kissat(setup));
        _fut_solver = ProcessWideThreadPool::get().addTask([&]() {
            const int* lits = _desc.getFormulaPayload(0);
            for (int i = 0; i < _desc.getFormulaPayloadSize(0); i++) {
                _solver->addLiteral(lits[i]);
            }
            _solver->diversify(0);
            _solver_result = _solver->solve(0, nullptr);
            _solver_done = true;
        });
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

    JobResult jsonToJobResult(nlohmann::json& json) const {
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
