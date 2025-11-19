
#pragma once

#include "app/sat/data/formula_compressor.hpp"
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

    //SAT call on original formula
    bool _base_job_submitted {false};
    volatile bool _base_job_done {false};
    bool _base_job_digested {false};
    nlohmann::json _base_job_submission;
    nlohmann::json _base_job_response;


    //Shared Sweeping Preprocessing
    bool _sweep_job_submitted {false};
    volatile bool _sweep_job_done {false};
    bool _sweep_job_digested {false};
    bool _sweep_job_has_improved_formula {false};
    bool _sweep_job_submitted_preprod {false};
    nlohmann::json _sweep_job_submission;
    nlohmann::json _sweep_job_response;


    //SAT call on preprocessed formula
    bool _preprod_job_submitted {false};
    volatile bool _preprod_job_done {false};
    bool _preprod_job_digested {false};
    nlohmann::json _preprod_job_submission;
    nlohmann::json _preprod_job_response;

    SatPreprocessor& _prepro;

public:
    SatPreprocessSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc),
        _prepro(*(new SatPreprocessor(_params, desc, _params.preprocessLingeling()))),
        _jobstr("#" + std::to_string(desc.getId())) {}
    ~SatPreprocessSolver() {
        if (!_params.terminateAbruptly()) delete &_prepro;
    }

    JobResult solve() {
        LOG(V1_WARN, "SATWP Starting SATWITHPRE app\n");
        _time_of_activation = Timer::elapsedSeconds();

        if (_params.preprocessBalancing() >= 0) submitBaseJob();

         /*
          *Starts the Kissat & Lingeling preprocessing
          */
        _prepro.init();

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = RESULT_UNKNOWN;

         /*
          *Waits for the preprocessing results
          */
        while (!isTimeoutHit()) {
            if (_base_job_done && !_base_job_digested) {
                LOG(V2_INFO, "SATWP base done\n");
                res = jsonToJobResult(_base_job_response, false);
                LOG(V2_INFO, "SATWP RESULT BASE done, result code %i\n", res.result);//grepped in postprocessing
                _base_job_digested = true;
                if (res.result != 0) break;
            }

            if (_sweep_job_done && !_sweep_job_digested) {
                LOG(V2_INFO, "SATWP SWEEP done\n");
                res = jsonToJobResult(_sweep_job_response, false); //eventually probably convert = true to reconstruct solution if necessary
                _sweep_job_digested = true;
                if (res.result==UNSAT) {
                    LOG(V2_INFO, "SATWP RESULT SWEEP reported UNSAT!\n");//grepped in postprocessing
                    break;
                }
                else if (res.result==IMPROVED) {
                    assert(res.getSolutionSize() > 0);
                    LOG(V3_VERB, "SATWP SWEEP has improved formula\n");
                    LOG(V3_VERB, "SATWP SWEEP reading json SolutionSize=%i\n", res.getSolutionSize());
                    _sweep_job_has_improved_formula = true;
                } else {
                    //todo: start preprocessed SAT job even if sweep didnt find an improvemnt
                    assert(res.result==UNKNOWN);
                    LOG(V1_WARN, "WARN: SATWP SWEEP did not improve formula! Leaving all resources to the running base SAT job, not starting a preprocessed job\n");
                }
            }

            if (_preprod_job_done && !_preprod_job_digested) {
                LOG(V2_INFO, "SATWP RESULT PREPROD-SAT done\n");//grepped in postprocessing
                res = jsonToJobResult(_preprod_job_response, true);
                _preprod_job_digested = true;
                if (res.result != 0) break;
            }

            if (_prepro.done()) {
                // Preprocess solver(s) terminated.
                LOG(V2_INFO, "SATWP sequential preprocessor done\n");
                if (_prepro.getResultCode() != 0) {
                    LOG(V2_INFO, "SATWP RESULT SEQ-PREPRO reported result %i\n", _prepro.getResultCode());//grepped in postprocessing
                    res.result = _prepro.getResultCode();
                    res.setSolution(std::move(_prepro.getSolution()));
                    break;
                }

                //if sequential preprocessing was unsuccessful, we still might want to schedule distributed sweeping as another preprocessing step
                //in that case that we need to get the original formula, as prepro didnt report any
                //before that: check how often preprocessor makes no progress at all, grep for SKIPPING
                //todo: start sweep even if prepro didnt improve anything?
                if (_params.preprocessSweep.val && ! _prepro.hasPreprocessedFormula()) {
                    LOG(V1_WARN, "SATWP WARN preprocessor done, didnt find any improvement. For now: SKIPPING Sweep. Future: May still want to schedule sweep now.\n");
                }
            }

            //old route without sweep: continue directly from prepro to prepro'd SAT
            if (_prepro.hasPreprocessedFormula() && ! _params.preprocessSweep.val) {
                LOG(V3_VERB, "SATWP submit preprocessed SAT task, skip SWEEP\n");
                submitPreprocessedJob(_prepro.extractPreprocessedFormula());
            }

            //new route: continue from prepro to SWEEP
            //does NOT yet start the retraction of the base job, as we are still doing preprocessing
            if (_prepro.hasPreprocessedFormula() && _params.preprocessSweep.val && ! _sweep_job_submitted) {
                LOG(V3_VERB, "SATWP Submit SWEEP\n");
                submitSweepJob(_prepro.extractPreprocessedFormula());
            }

            //continue from successful SWEEP to preprocessed SAT
            if (_sweep_job_digested &&  !_sweep_job_submitted_preprod &&_sweep_job_has_improved_formula) {
                LOG(V3_VERB, "SATWP submit preprocessed SAT task (via providing SWEEP results)\n");
                submitPreprocessedJob(res.extractSolution());
                _sweep_job_submitted_preprod = true; //prevent multiple submissions from SWEEP to SAT
            }

            //continue from unsuccessfull SWEEP to preprocessed SAT
            // if (_sweep_job_digested && !_sweep_job_submitted_preprod && ! _sweep_job_has_improved_formula) {

            // }

            if (!_base_job_done && _time_of_retraction_end > 0 && Timer::elapsedSeconds() >= _time_of_retraction_end) {
                LOG(V3_VERB, "SATWP Interrupting base job due to retraction\n");
                interrupt(_base_job_submission, _base_job_done);
            }
            usleep(3*1000);
        }

        LOG(V3_VERB, "SATWP exited main solving loop. Now interrupting remaining jobs.\n");

        // Terminate sub-jobs
        if (_base_job_submitted && !_base_job_done) {
            interrupt(_base_job_submission, _base_job_done);
            while (!_base_job_done) usleep(5*1000);
        }
        if (_preprod_job_submitted && !_preprod_job_done) {
            interrupt(_preprod_job_submission, _preprod_job_done);
            while (!_preprod_job_done) usleep(5*1000);
        }
        if (_sweep_job_submitted && !_sweep_job_done) {
            interrupt(_sweep_job_submission, _sweep_job_done);
            while (!_sweep_job_done) usleep(5*1000);
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

        LOG(V3_VERB, "SATWP Starting Base Job: %d Vars\n", _desc.getAppConfiguration().fixedSizeEntryToInt("__NV"));
        LOG(V3_VERB, "SATWP Starting Base Job: %d Clauses\n", _desc.getAppConfiguration().fixedSizeEntryToInt("__NC"));

    	// int base_procs = 2;
        // json["max-demand"] = 2; //Test: Manually/hardcoded control number of PEs
        // LOG(V3_VERB, "SATWP Starting Base Job: %d MPI Processes/Ranks allocated\n", base_procs);

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


    void submitSweepJob(std::vector<int>&& fPre) {

        assert(fPre.size() > 2);
        int nbClauses = fPre.back(); fPre.pop_back();
        int nbVars = fPre.back(); fPre.pop_back();
        // size_t preprocessedSize = fPre.size();

        if (_params.compressFormula()) {
            auto out = FormulaCompressor::compress(fPre.data(), fPre.size(), 0, 0);
            fPre = std::move(*out.vec);
        }

        //NOT copying the retraction code, because we are not retracting the base job yet

        // Prepare job submission data
        auto& json = _sweep_job_submission;
        json = {
            {"user", "sweep-" + std::string(toStr())},
            {"name", std::string(toStr())+":prepro:sweep"},
            {"priority", _params.preprocessSweepPriority()},
            {"application", "SWEEP"},
        };
        if (_params.crossJobCommunication()) json["group-id"] = _desc.getGroupId();
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(), std::move(fPre));
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(nbVars);
        json["configuration"]["__NC"] = std::to_string(nbClauses);
        // if (_params.preprocessBalancing() == 1)
            // json["configuration"]["__growprd"] = std::to_string(_retraction_round_duration);
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(_desc.getWallclockLimit() - getAgeSinceActivation()) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(_desc.getCpuLimit() - getAgeSinceActivation()) + "s";


    	// int sweep_procs = 2;
        // json["max-demand"] = sweep_procs; //Test: SWEEP with only two PEs/Workers
        // json["configuration"]["t"] = std::to_string(2); //Test: Only two threads per sweep worker
        // LOG(V3_VERB, "SATWP Starting SWEEP Job: %d MPI Processes/Ranks allocated\n", sweep_procs);

        LOG(V3_VERB, "SATWP Starting SWEEP Job: %d Vars\n", nbVars);
        LOG(V3_VERB, "SATWP Starting SWEEP Job: %d Clauses\n", nbClauses);

        // Obtain API and submit the job
        auto copiedJson = json;
        auto retcode = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            _sweep_job_response = std::move(response);
            _sweep_job_done = true;
        });
        if (retcode != JsonInterface::ACCEPT) return;

        _sweep_job_submitted = true;

    }

    void submitPreprocessedJob(std::vector<int>&& fPre) {

        assert(fPre.size() > 2);
        int nbClauses = fPre.back(); fPre.pop_back();
        int nbVars = fPre.back(); fPre.pop_back();
        size_t preprocessedSize = fPre.size();

        if (_params.compressFormula()) {
            auto out = FormulaCompressor::compress(fPre.data(), fPre.size(), 0, 0);
            fPre = std::move(*out.vec);
        }

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
        auto& json = _preprod_job_submission;
        json = {
            {"user", "sat-" + std::string(toStr())},
            {"name", std::string(toStr())+":preprod"},
            {"priority", _params.preprocessJobPriority()},
            {"application", "SAT"},
        };
        if (_params.crossJobCommunication()) json["group-id"] = _desc.getGroupId();
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(), std::move(fPre));
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(nbVars);
        json["configuration"]["__NC"] = std::to_string(nbClauses);
        if (_params.preprocessBalancing() == 1)
            json["configuration"]["__growprd"] = std::to_string(_retraction_round_duration);
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(_desc.getWallclockLimit() - getAgeSinceActivation()) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(_desc.getCpuLimit() - getAgeSinceActivation()) + "s";


        LOG(V3_VERB, "SATWP Starting SAT Job: %d Vars\n", nbVars);
        LOG(V3_VERB, "SATWP Starting SAT Job: %d Clauses\n", nbClauses);

        // Obtain API and submit the job
        auto copiedJson = json;
        auto retcode = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            _preprod_job_response = std::move(response);
            _preprod_job_done = true;
        });
        if (retcode != JsonInterface::ACCEPT) return;

        _preprod_job_submitted = true;
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
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = json["result"]["resultcode"];
        LOG(V3_VERB, "SATWP Extract result of %s  (result code %i)\n", json["name"].get<std::string>().c_str(), res.result);
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
        // LOG(V3_VERB, "SATWP %s extracted\n", json["name"].get<std::string>().c_str());
        LOG(V3_VERB, "SATWP %s extracted  Solution Size %i\n", json["name"].get<std::string>().c_str(), res.getSolutionSize());
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
