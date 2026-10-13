
#ifndef MALLOB_MUS_SOLVER_HPP
#define MALLOB_MUS_SOLVER_HPP

#include "app/app_terminate_checker.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/parse/cnf_util.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/static_store.hpp"

class MusSolver {
private:
    const Parameters _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata
    AppTerminateChecker _term;

    MessageSubscription _sub_xtcs_incoming_clauses;
    Mutex _mtx_incoming_messages;
    std::list<MessageHandle> _incoming_messages;

    const std::vector<int> _input_cnf;
    std::vector<int> _cnf_w_assumptions;
    nlohmann::json _base_json;

    std::vector<char> _assumptions_status;
    int _first_assumption_idx{0};

    int _sub_job_id {-1};
    std::atomic_int _child_job_root_rank {-1};

    JobResult _result;

public:
    // Initializes the solver instance and parses the description's formula.
    MusSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params),
        _api(api),
        _desc(desc),
        _term(_params, _desc),
        _sub_xtcs_incoming_clauses(MSG_SEND_APP_DATA_TO_CLIENT_JOB, [&](MessageHandle& h) {
            auto lock = _mtx_incoming_messages.getLock();
            _incoming_messages.push_back(h);
        }),
        _input_cnf(CnfUtil::getCnfFromJobDescription(desc, true))
    {
        LOG(V2_INFO, "Mallob client-side MUS solver, by Niccolò Rigi-Luperti & Dominik Schreiber\n");
        _jobstr = "MUS#" + std::to_string(_desc.getId());
    }
    MusSolver(MusSolver&& other) = delete;
    ~MusSolver() {}


    JobResult solve() {
        _result.id = _desc.getId();
        _result.revision = 0;
        _result.result = RESULT_UNKNOWN;
        LOG(V2_INFO, "MUS solve()\n");
        LOG(V2_INFO, "Input %i vars %i clauses\n", nbInputVars(), nbInputClauses());
        _assumptions_status.resize(nbInputClauses(),0);
        _first_assumption_idx = nbInputVars() + 1;
        _cnf_w_assumptions.clear();
        int new_asm_var = _first_assumption_idx;
        for (int lit : _input_cnf) {
            if (lit == 0) {
                _cnf_w_assumptions.push_back(-new_asm_var);
                new_asm_var++;
            }
           _cnf_w_assumptions.push_back(lit);
        }
        //start SAT job with assumption-filled cnf
        submitScatterSATJob();
        return _result;
    }

private:
    void submitScatterSATJob() {
        // Prepare job submission data
        auto& json = _base_json;
        json = {
            {"user", std::string(toStr())},
            {"name", std::string(toStr())+":" + "SATbyMUS" + ":job"},
            {"priority", "1"},
            {"application", "SAT"},
            {"group-id", "1"},
            {"incremental", false}
        };

        auto f = _cnf_w_assumptions;
        StaticStore<std::vector<int>>::insert(json["name"].get<std::string>(), std::move(f));
        json["internalliterals"] = json["name"].get<std::string>();
        json["configuration"]["__NV"] = std::to_string(nbInputVars());
        json["configuration"]["__NC"] = std::to_string(nbInputClauses());
        if (_desc.getWallclockLimit() > 0)
            json["wallclock-limit"] = std::to_string(std::max(0.001f, _desc.getWallclockLimit())) + "s";
        if (_desc.getCpuLimit() > 0)
            json["cpu-limit"] = std::to_string(std::max(0.001f, _desc.getCpuLimit())) + "s";
        json["configuration"]["scatter"] = "true";
        // std::string opts;
        // if (json["configuration"].count("options"))
            // opts = json["configuration"]["options"].get<std::string>();
        // if (!opts.empty()) json["configuration"]["options"] = opts;

        auto copiedJson = json;
        auto result = _api.submit(copiedJson, [&](nlohmann::json& response) {
            // Job done
            auto res = jsonToJobResult(response);
            // if (res.result == RESULT_SAT) _result = SAT;
            // else if (res.result == RESULT_UNSAT) _result = UNSAT;
            // else if (res.result == RESULT_SIMPLIFIED) _result = SIMPLIFIED;
            // else _result = NONE;
        }, &_sub_job_id, [&](int rootRank) {_child_job_root_rank = rootRank;});
        if (result != JsonInterface::Result::ACCEPT) {
            LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
            abort();
        }
    }

    JobResult jsonToJobResult(nlohmann::json& json) {
        LOG(V2_INFO, "MUS extract result of %s\n", json["name"].get<std::string>().c_str());
        _result.result = json["result"]["resultcode"];
        if (_result.result == RESULT_UNKNOWN) return _result;
        auto solution = json["result"]["solution"].get<std::vector<int>>();
        _result.setSolution(std::move(solution));
        LOG(V2_INFO, "MUS %s extracted\n", json["name"].get<std::string>().c_str());
        return _result;
    }

    std::string _jobstr;
    const char* toStr() const {
        return _jobstr.c_str();
    }

    int nbInputVars() const {
        assert(_input_cnf.size() >= 2);
        return _input_cnf[_input_cnf.size() - 2];
    }
    int nbInputClauses() const {
        assert(_input_cnf.size() >= 2);
        return _input_cnf[_input_cnf.size() - 1];
    }
};




#endif //MALLOB_MUS_SOLVER_HPP