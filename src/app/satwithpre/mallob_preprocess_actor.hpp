
#pragma once

#include <cmath>
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
#include "app/sweep/sweep_job.hpp"
#include <sys/stat.h>
#include <unistd.h>

class MallobPreprocessActor : public SatPreprocessActor {

public:
    enum MallobJobType {SATSOLVER, SWEEPER};

private:
    const JobDescription& _desc; // contains our instance to solve and all metadata
    APIConnector& _api; // for submitting jobs to Mallob
    const int _job_id;
    const float _time_of_activation;
    const std::string _group_id;
    const std::string _option_overrides;
    const MallobJobType _type;

    nlohmann::json _base_json;
    int _sub_job_id {-1};
    
    //intermediate data from Sweeping used for model reconstruction
    std::vector<int> _sweep_units{};
    std::vector<int> _sweep_eqs{};

public:
    MallobPreprocessActor(const Parameters& params, const JobDescription& desc, const std::string& name,
            APIConnector& api, std::vector<int>&& formula, float timeOfActivation,
            MallobJobType type, const std::string& groupId, const std::string& optionOverrides) :
        SatPreprocessActor(params, name, std::move(formula)), _desc(desc), _api(api),
            _job_id(desc.getId()), _time_of_activation(timeOfActivation),
            _group_id(groupId), _option_overrides(optionOverrides), _type(type) {

        static int _actor_counter = 1;

        _jobstr = "#" + std::to_string(_job_id) + ":mal:" + std::to_string(_actor_counter++);
        _proof_format = _type == SATSOLVER ? "palrup" : "";
    }
    ~MallobPreprocessActor() {}

    void preprocessAsync() override {
        submitJob();
    }
    
    // void getRepr(int var, std::vector<int> &repr) {
       // while (repr[var]!=var) {
           
       // }
    // }
    
    // static unsigned VAR_TO_LIT(const int var) {
        // unsigned lit = ((unsigned)std::abs(var)) << 1;
        // if (var < 0) {
            // lit++;
        // }
        // return lit;
    // }
    
    // static unsigned NOT_LIT(const unsigned lit) {
        // return lit ^ 1u;
    // }
    
    // static unsigned getRepr(unsigned lit, std::vector<unsigned> &repr) {
        // unsigned res = repr[lit];
        // while (res != lit) {
            // lit = res;
            // res = repr[lit];
        // }
        // return res;
    // }
    
    static int getReprVar(int var, std::vector<int> &repr) {
        if (repr[var]==0) {
            return 0;
        }
        int sign = 1; 
        int res = repr[var];
        while (res != var) {
            LOG(V1_WARN, "SATWP %i --> %i \n", var, res);
            if (res < 0) {
                sign = -sign;
                res = -res;
            }
            var = res;
            res = repr[var];
        }
        return res * sign;
    }
    
    static int signof(int var) {
        if (var==0) return 0;
        if (var<0) return -1;
        return 1;
    }
    
    // static int LIT_TO_VAR(const unsigned lit) {
        // int var = lit >> 1;
        // if (lit & 1u) {
           // var = -var; 
        // }
        // return var;
    // }
    
    void reconstructSolution(std::vector<int>& model) override {
        if (_type == SWEEPER) {
            LOG(V0_CRIT, "SATWP Sweeper wants to reconstruct solution with given model size %i\n", model.size()-1);
            
            //Make all sweep units accessible by index
            std::vector<int> sweepunits(nbInputVars()+1, 0);
            std::sort(_sweep_units.begin(), _sweep_units.end());
            for (int unit : _sweep_units) {
                sweepunits[std::abs(unit)] = unit;
                LOG(V3_VERB, "SATWP Sweeper unit %i\n", unit);
            }
            
            //Make all sweep equivalences accessible by index
            //Use unsigned format, it makes signed-ness much easier to handle
            // std::vector<unsigned> representatives(nbInputVars()+1, 0);
            std::vector<int> representatives(nbInputVars()+1);
            for (int i=0; i<representatives.size(); i++) {
                representatives[i]=i;
            }
            for (int i=0; i<_sweep_eqs.size(); i+=2) {
                int v1 = _sweep_eqs[i];
                int v2 = _sweep_eqs[i+1];
                LOG(V3_VERB, "SATWP Sweeper eq %i %i\n", _sweep_eqs[i], _sweep_eqs[i+1]);
                assert(std::abs(v1)<std::abs(v2));
                // unsigned lit1 = VAR_TO_LIT(v1);
                // unsigned lit2 = VAR_TO_LIT(v2);
                // unsigned notlit1 = NOT_LIT(lit1);
                // unsigned notlit2 = NOT_LIT(lit2);
                // representatives[lit2]=lit1;
                // representatives[notlit2]=notlit1;
                if (v2 < 0) {
                    v2 = -v2;
                    v1 = -v1;
                }
                representatives[v2]=v1;
            }
            
            model.resize(nbInputVars()+1);
            //Order of resolving the polarity of each variable:
            //  1. Sweep unit
            //  2. Sweep representative into Sweep unit
            //  3. Sweep representative into model lit
            //  4. model lit (unchanged)
            for (int var = 1; var <= nbInputVars() ; var++) {
                if (const int sweepLit = sweepunits[var]; sweepLit != 0) {
                    //Case 1: Sweep knows the unit value
                    if (model[var] != sweepLit) {
                        LOG(V1_WARN, "SATWP [WARN] var %i : Sweep lit (%i) != model lit (%i) \n", var, sweepLit, model[var]);
                    }
                    LOG(V1_WARN, "SATWP sweepLit %i \n", sweepLit);
                    model[var] = sweepLit;
                } else if (int reprVar = getReprVar(var, representatives); reprVar != var) {
                    const int signToRep = signof(reprVar);
                    reprVar = std::abs(reprVar);
                    const int sweepReprLit = sweepunits[reprVar] * signToRep;
                    const int sweepReprSign = signof(sweepReprLit);
                    if (sweepReprSign!= 0) {
                        //Case 2: Sweep knows a representative, and it knows its value
                        if (sweepReprLit != model[reprVar]) {
                            LOG(V1_WARN, "SATWP [WARN] var %i : Sweep repr lit (%i) != model repr lit (%i) \n", var, sweepReprLit, model[reprVar]);
                        }
                        const int deducedLit = var * signToRep * sweepReprSign;
                        if (deducedLit != model[var]) {
                            LOG(V1_WARN, "SATWP [WARN] var %i : Sweep deduced lit (%i) != model lit (%i) \n", var, deducedLit, model[var]);
                        }
                        LOG(V1_WARN, "SATWP deducedSweepLit %i \n", deducedLit);
                        model[var] = deducedLit;
                    } else {
                        //Case 3: Sweep knows a representative, but not its value
                        const int modelReprLit = model[reprVar];
                        const int modelReprSign = signof(modelReprLit);
                        const int deducedLit = var * signToRep * modelReprSign;
                        if (deducedLit != model[var]) {
                            LOG(V1_WARN, "SATWP [WARN] var %i : Sweep model deduced lit (%i) != model lit (%i) \n", var, deducedLit, model[var]);
                        }
                        LOG(V1_WARN, "SATWP deducedModelLit %i \n", deducedLit);
                        model[var] = deducedLit;
                    }
                } else {
                    LOG(V1_WARN, "SATWP model %i \n", model[var]);
                }
                // LOG(V3_VERB, "SATWP Sweeper sees var %i == %i (%i)\n", var, model[var], stored_unit);
            }
        }
        //Nothing to do with type SATSOLVER
    }

    void interrupt() override {
        interrupt(_base_json);
    }

    // some processes may still be writing, so we need to wait until moving is possible
    bool rename_proof(int i, std::string cnfSrc = "", std::string proofSrc = "") override {
        std::string src = _params.proofDirectory() + "/tmp/" + _name + "." + _proof_format;
        for (int attempt = 0; attempt < 100 && std::filesystem::exists(src); attempt++) {
            bool pending = false;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
                std::string filename = entry.path().filename().string();
                if (!filename.empty() && filename.back() == '~') { pending = true; break; }
            }
            if (!pending) break;
            usleep(1000 * 100); // 100ms
        }
        createMissingDirectoriesAndFiles(src + "/proof#" + std::to_string(_sub_job_id));
        return SatPreprocessActor::rename_proof(i);
    }

private:
    void submitJob() {
        // Prepare job submission data
        auto& json = _base_json;
        json = {
            {"user", std::string(toStr())},
            {"name", std::string(toStr())+":" + (_type == SWEEPER ? "SWEEP" : "SAT") + ":job"},
            {"priority", _params.preprocessSweepPriority()},
            {"application", _type == SWEEPER ? "SWEEP" : "SAT"},
            {"group-id", _group_id},
            {"configuration", {{"options", _option_overrides}}}
        };

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

        std::string opts;
        if (json["configuration"].count("options"))
            opts = json["configuration"]["options"].get<std::string>();
        if (_type == SATSOLVER && _params.overrideSatOptions.isSet())
            opts += " " + _params.overrideSatOptions();
        if (_params.savePreprocessingProofs())
            opts += " -palrup=1 -proof-dir=" + _params.proofDirectory() + "/tmp/" + _name + "." + _proof_format;
        if (!opts.empty()) json["configuration"]["options"] = opts;
        applySuccessiveGrowth(json);

        auto copiedJson = json;
        auto result = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            auto res = jsonToJobResult(response);
            if (res.result == RESULT_SAT) _result = SAT;
            else if (res.result == RESULT_UNSAT) _result = UNSAT;
            else if (res.result == RESULT_SIMPLIFIED) _result = SIMPLIFIED;
            else _result = NONE;
        }, &_sub_job_id);
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
            if (_type == SWEEPER) {
                //Sweep returns three arrays in its result vector, [units, equivalences, formula], 
                //we store units and equivalences for model reconstruction, and pass on the formula
                SweepJob::SweepResult sweepRes = SweepJob::deserializeSweepResult(solution);
                _sweep_units = std::move(sweepRes.units);
                _sweep_eqs   = std::move(sweepRes.eqs);
                LOG(V3_VERB, "SATWP %s : Sweepunits %i\n", toStr(), _sweep_units.size());
                LOG(V3_VERB, "SATWP %s : Sweepeqs   %i\n", toStr(), _sweep_eqs.size());
                //Trim the solution-vector to just the formula, to make the sweep splicing transparent to following code
                solution = std::move(sweepRes.formula);
            }
            _output_cnf = std::move(solution);
            //already contains metadata #vals and #clauses in the last two entries
        }
        //TODO: Is there even anything left in solution at this point, since we already moved it to _output_cnf?
        //      and, does it even matter? because nothing is done with this set solution...
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

    void createMissingDirectoriesAndFiles(const std::string& src) {
        double maxNumSolvers = MyMpi::size(MPI_COMM_WORLD) * _params.numThreadsPerProcess();
        int lastCreatedHierarchy = -1;
        for (int i = 0; i < maxNumSolvers; i++) {
            int hierarchy = (int) (i / std::ceil(std::sqrt(maxNumSolvers)));
            if (hierarchy > lastCreatedHierarchy) {
                FileUtils::mkdir(src + "/" + std::to_string(hierarchy));
                lastCreatedHierarchy = hierarchy;
            }
            FileUtils::mkdir(src + "/" + std::to_string(hierarchy) + "/" + std::to_string(i));
            FileUtils::create(src + "/" + std::to_string(hierarchy) + "/" + std::to_string(i) + "/out.palrup");
        }
    }
};
