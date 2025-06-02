
#pragma once

#include <algorithm>
#include <cstdlib>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "app/sat/data/model_string_compressor.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "sat_job_stream_processor.hpp"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/static_store.hpp"
#include "util/assert.hpp"
#include "util/sys/timer.hpp"

class MallobSatJobStreamProcessor : public SatJobStreamProcessor {

private:
    const Parameters& _params;
    APIConnector& _api;
    int _stream_id;

    int _nb_vars {0};
    int _nb_clauses {0};

    bool _incremental {true};
    const std::string _username;
    std::string _base_job_name;
    nlohmann::json _json_base;
    int _subjob_counter {0};
    nlohmann::json _json_result;
    std::string _expected_result_job_name;

    bool _task_pending {false};
    int _pending_rev {-1};
    bool _pending_task_interrupted {false};

    bool _began_nontrivial_solving {false};
    std::vector<int> _backlog_lits;

public:
    MallobSatJobStreamProcessor(const Parameters& params, APIConnector& api, JobDescription& desc,
            const std::string& baseUserName, int streamId, bool incremental, Synchronizer& sync) :
        SatJobStreamProcessor(sync), _params(params), _api(api), _stream_id(streamId),
        _incremental(incremental), _username(baseUserName) {}

    ~MallobSatJobStreamProcessor() override {}

    virtual void setName(const std::string& baseName) override {
        _name = baseName + ":mal";
    }
    void setInitialSize(int nbVars, int nbClauses) {
        _nb_vars = nbVars;
        _nb_clauses = nbClauses;
    }

    virtual void process(SatTask& task) override {

        if (!_began_nontrivial_solving) {
            // If no distributed job was submitted yet, we try to avoid this overhead;
            // we wait for a short while if a more lightweight solver finds a solution immediately.
            auto time = Timer::elapsedSeconds();
            _backlog_lits.insert(_backlog_lits.end(), task.lits.begin(), task.lits.end());
            time = Timer::elapsedSeconds() - time;
            usleep(1'000'000 * std::max(0.0, 0.05 - time)); // 50 ms minus the time taken to copy the literals
            if (_terminator(task.rev)) {
                return; // Task has become obsolete in the meantime, so skip solving
            }
            // Task is not (yet) obsolete after the wait, so we now begin proper distributed solving
            LOG(V2_INFO, "%s awakes for rev. %i\n", _name.c_str(), task.rev);
            _began_nontrivial_solving = true;
            task.lits = std::move(_backlog_lits);

            _base_job_name = "satjob-" + std::to_string(_stream_id) + "-rev-";
            _json_base = nlohmann::json {
                {"user", _username},
                {"incremental", _incremental},
                {"priority", 1},
                {"application", "SAT"}
            };
            _json_base["files"] = std::vector<std::string>();
            if (!_json_base["configuration"].count("__XL"))
                _json_base["configuration"]["__XL"] = "-1";
            if (!_json_base["configuration"].count("__XU"))
                _json_base["configuration"]["__XU"] = "-1";
            _json_base["configuration"]["__NV"] = std::to_string(_nb_vars);
            _json_base["configuration"]["__NC"] = std::to_string(_nb_clauses);
        }

        auto& newLiterals = task.lits;
        const auto& assumptions = task.assumptions;
        auto chksum = task.chksum;
        const auto& descriptionLabel = task.descLabel;
        float priority = task.priority;

        if (_params.useChecksums()) _json_base["checksum"] = {chksum.count(), chksum.get()};

        if (_incremental && _json_base.contains("name")) {
            _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        }
        _json_base["priority"] = priority > 0 ? priority : 1;
        const int subjob = _subjob_counter++;
        _json_base["name"] = _base_job_name + std::to_string(subjob);
        /*if (_params.maxSatWriteJobLiterals()) {
            std::ofstream ofs(_params.logDirectory() + "/satjobstream.joblits." + _json_base["name"].get<std::string>());
            for (int lit : newLiterals) {
                ofs << lit << " ";
                if (lit == 0) ofs << std::endl;
            }
            ofs = std::ofstream(_params.logDirectory() + "/satjobstream.jobassumptions." + _json_base["name"].get<std::string>());
            for (int lit : assumptions) {
                ofs << lit << " 0" << std::endl;
            }
        }*/
        nlohmann::json copy(_json_base);
        StaticStore<std::vector<int>>::insert(_json_base["name"].get<std::string>(), std::move(newLiterals));
        copy["internalliterals"] = _json_base["name"].get<std::string>();
        copy["assumptions"] = assumptions;
        if (!descriptionLabel.empty()) {
            copy["description-id"] = descriptionLabel;
        }
        _expected_result_job_name = copy["name"].get<std::string>();

        _task_pending = true;
        _pending_rev = task.rev;
        _pending_task_interrupted = false;
        try {
            auto response = _api.submit(copy, [&, rev = _pending_rev, subjob](nlohmann::json& result) {

                if (result["name"].get<std::string>() != _expected_result_job_name) {
                    LOG(V0_CRIT, "[ERROR] Result for unexpected job \"%s\" (expected: %s)!\n",
                        result["name"].get<std::string>().c_str(), _expected_result_job_name.c_str());
                    abort();
                }

                int resultCode = result["result"]["resultcode"];
                std::vector<int> solution;
                if (resultCode == 10 && _params.compressModels()) {
                    solution = ModelStringCompressor::decompress(result["result"]["solution"].get<std::string>());
                } else {
                    solution = result["result"]["solution"].get<std::vector<int>>();
                }
                bool winner = concludeRevision(rev, resultCode, std::move(solution));
                if (winner) LOG(V2_INFO, "%s rev. %i (internally %i) won with res=%i\n", _name.c_str(), rev, subjob, resultCode);
                _task_pending = false;
            });
            if (response == JsonInterface::Result::DISCARD) {
                concludeRevision(_pending_rev, 0, {});
                _task_pending = false;
            }
        } catch (...) {
            LOG(V0_CRIT, "[ERROR] uncaught exception while submitting JSON\n");
            abort();
        }

        unsigned long sleepInterval {1};
        while (checkTaskPending(task.rev)) {
            usleep(sleepInterval);
            sleepInterval = std::min(2500UL, (unsigned long) std::ceil(1.2*sleepInterval));
        }
    }

    virtual void finalize() override {
        SatJobStreamProcessor::finalize();
        if (!_began_nontrivial_solving) return;
        while (_task_pending) usleep(3000);
        if (!_incremental) return;
        if (!_json_base.contains("name")) return;
        _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        _json_base["name"] = _base_job_name + std::to_string(_subjob_counter++);
        nlohmann::json copy(_json_base);
        copy["done"] = true;
        // The callback is never called.
        LOG(V2_INFO, "%s closing API\n", _name.c_str());
        _api.submit(copy, [&](nlohmann::json& result) {assert(false);});
        LOG(V2_INFO, "%s closed API\n", _name.c_str());
    }

    void setGroupId(const std::string& groupId, int minVar = -1, int maxVar = -1) {
        LOG(V2_INFO, "MAXSAT %s group ID %s V=[%i,%i]\n", _base_job_name.c_str(), groupId.c_str(), minVar, maxVar);
        _json_base["group-id"] = groupId;
        _json_base["configuration"]["__XL"] = std::to_string(minVar);
        _json_base["configuration"]["__XU"] = std::to_string(maxVar);
    }
    void setInnerObjective(const std::string& objective) {
        _json_base["configuration"]["__OBJ"] = objective;
    }

    const std::string& getUserName() const {
        return _username;
    }

private:
        bool checkTaskPending(int rev) {
        if (!_task_pending) return false;
        if (_pending_task_interrupted) return true;
        if (!_terminator(rev)) return true;

        _pending_task_interrupted = true;
        nlohmann::json jsonInterrupt {
            {"name", _json_base["name"]},
            {"user", _json_base["user"]},
            {"application", _json_base["application"]},
            {"incremental", _json_base["incremental"]},
            {"interrupt", true}
        };
        // In this particular case, the callback is never called.
        // Instead, the callback of the job's original submission is called.
        auto response = _api.submit(jsonInterrupt, [&](nlohmann::json& result) {assert(false);});
        if (response == JsonInterface::Result::DISCARD) {
            concludeRevision(_pending_rev, 0, {});
            _task_pending = false;
            return false;
        }
        return true;
    }
};
