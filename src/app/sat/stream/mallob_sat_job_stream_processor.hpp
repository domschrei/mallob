
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "app/sat/data/formula_compressor.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "sat_job_stream_processor.hpp"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/static_store.hpp"
#include "util/sys/timer.hpp"

class MallobSatJobStreamProcessor : public SatJobStreamProcessor {

private:
    const Parameters& _params;
    APIConnector& _api;
    int _stream_id;
    long _nontrivial_wait_millis_initial;
    long _nontrivial_wait_millis_subsequent;

    int _nb_vars {0};
    int _nb_clauses {0};

    bool _incremental {true};
    const std::string _username;
    std::string _base_job_name;
    nlohmann::json _json_base;
    int _subjob_counter {0};
    nlohmann::json _json_result;
    std::string _expected_result_job_name;

    volatile bool _task_pending {false};
    int _pending_rev {-1};
    bool _pending_task_interrupted {false};

    bool _began_nontrivial_solving {false};
    bool _retrieve_complete_task {false};
    SatTask _backlog_task {SatTask::RAW, {}, {}, 0, 0};
    bool _initialized_backlog_task {false};
    bool _finalized {false};

    DTaskTracker* _dtask_tracker {nullptr};
    std::shared_ptr<DTaskTracker::DTask> _dtask;

    int _last_won_rev {-2};

public:
    MallobSatJobStreamProcessor(const Parameters& params, APIConnector& api, JobDescription& desc,
            const std::string& baseUserName, int streamId, bool incremental, Synchronizer& sync) :
        SatJobStreamProcessor(sync), _params(params), _api(api), _stream_id(streamId),
        _nontrivial_wait_millis_initial(params.internalStreamProcessor() ? params.nontrivialSolvingDelayInitial() : 0),
        _nontrivial_wait_millis_subsequent(params.internalStreamProcessor() ? params.nontrivialSolvingDelaySubsequent() : 0),
        _incremental(incremental), _username(baseUserName)
        //,_job_slot(new JobSlotRegistry::JobSlot(_username, [&]() {signalReinitialization();})) 
            {
        initJson();
    }

    void initJson() {
        int jobSlots = _params.jobSlots() > 0 ? _params.jobSlots() : MyMpi::size(MPI_COMM_WORLD);
        int numConcStreams = std::min(jobSlots, MyMpi::size(MPI_COMM_WORLD));
        _base_job_name = "satjob-" + std::to_string(_stream_id) + "-rev-";

        _json_base = {};
        _json_base["user"] = _username;
        _json_base["incremental"] = _incremental;
        _json_base["priority"] = 1;
        _json_base["application"] = "SAT";
        _json_base["files"] = std::vector<std::string>();
        if (!_json_base["configuration"].count("__XL"))
            _json_base["configuration"]["__XL"] = "-1";
        if (!_json_base["configuration"].count("__XU"))
            _json_base["configuration"]["__XU"] = "-1";
        _json_base["configuration"]["__EO"] = std::to_string(_stream_id);
        _json_base["configuration"]["__EM"] = std::to_string(numConcStreams);
    }

    ~MallobSatJobStreamProcessor() override {}

    virtual void setName(const std::string& baseName) override {
        _name = baseName + ":mal";
    }
    void setInitialSize(int nbVars, int nbClauses) {
        _nb_vars = nbVars;
        _nb_clauses = nbClauses;
    }
    void setDTaskTracker(DTaskTracker& tracker) {
        _dtask_tracker = &tracker;
    }

    virtual void loop() override {
        if (_dtask && _dtask->evicted) yield();
    }

    virtual void process(SatTask& task) override {

        auto time = Timer::elapsedSeconds();
        if (!_initialized_backlog_task) {
            _backlog_task.type = task.type;
            _initialized_backlog_task = true;
        }
        if (_retrieve_complete_task) {
            // Flag set to retrieve the complete task again
            _retrieve_complete_task = false;
            _backlog_task = _cb_retrieve_full_task();
            if (_backlog_task.rev < task.rev) {
                // Incoming task directly (!) succeeds the retrieved backlog task: just integrate
                assert(_backlog_task.rev + 1 == task.rev);
                _backlog_task.integrate(task);
            } else if (_backlog_task.rev == task.rev) {
                // Incoming task is exactly the retrieved backlog task
                assert(_backlog_task.assumptions == task.assumptions);
            } else {
                // Incoming task precedes the retrieved backlog task: incoming task obsolete!
                LOG(V3_VERB, "%s task with int. rev %i obsolete\n", _name.c_str(), task.rev);
                return;
            }
        } else {
            _backlog_task.integrate(task);
        }
        auto& t = _backlog_task;
        if (t.nbVars >= 0) _nb_vars = t.nbVars;
        if (t.nbClauses >= 0) _nb_clauses = t.nbClauses;
        if (t.lits.size() > 0) assert(_nb_clauses > 0);

        LOG(V5_DEBG, "%s attempting to solve task ...\n", _name.c_str());

        if (_task_pending) {
            // A previous interruption is still ongoing.
            // We need to wait for it to complete before we can submit the next revision.
            LOG(V4_VVER, "%s pending ...\n", _name.c_str());
            unsigned long sleepInterval {1};
            while (_task_pending) {
                usleep(sleepInterval);
                sleepInterval = std::min(2500UL, (unsigned long) std::ceil(1.2*sleepInterval));
            }
            LOG(V4_VVER, "%s ... ready\n", _name.c_str());
        }

        if (_dtask && _dtask->evicted) {
            yield();
            return;
        }

        if (!_began_nontrivial_solving && _nontrivial_wait_millis_initial > 0) {
            LOG(V4_VVER, "%s sleep initially\n", _name.c_str());
            time = Timer::elapsedSeconds() - time;
            // X ms minus the time taken to copy the literals
            usleep(1'000'000 * std::max(0.0, 0.001 * _nontrivial_wait_millis_initial - time));
            if (_terminator(t.rev)) return; // Task has become obsolete in the meantime, so skip solving

        } else if (_last_won_rev < t.rev-1 && _nontrivial_wait_millis_subsequent > 0) {
            LOG(V4_VVER, "%s sleep (last won: %i, now: %i)\n", _name.c_str(), _last_won_rev, t.rev);
            time = Timer::elapsedSeconds() - time;
            // X ms minus the time taken to copy the literals
            usleep(1'000'000 * std::max(0.0, 0.001 * _nontrivial_wait_millis_subsequent - time));
            if (_terminator(t.rev)) return; // Task has become obsolete in the meantime, so skip solving
        }

        if (!_finalized && !_began_nontrivial_solving) {

            // Task is not (yet) obsolete after the wait, so we now begin proper distributed solving
            LOG(V2_INFO, "%s awakes for rev. %i (V=%i C=%i L=%i)\n", _name.c_str(), t.rev,
                _nb_vars, _nb_clauses, t.lits.size());
            _began_nontrivial_solving = true;
            _dtask = _dtask_tracker->acquireSlot();

            _json_base["configuration"]["__NV"] = std::to_string(_nb_vars);
            _json_base["configuration"]["__NC"] = std::to_string(_nb_clauses);
        }

        auto& newLiterals = t.lits;
        const auto& assumptions = t.assumptions;
        auto chksum = t.chksum;
        const auto& descriptionLabel = t.descLabel;
        float priority = t.priority;

        if (_finalized || !_began_nontrivial_solving) return;

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

        if (_params.compressFormula()) {
            auto out = FormulaCompressor::compress(newLiterals.data(), newLiterals.size(),
                assumptions.data(), assumptions.size());
            newLiterals = std::move(*out.vec);
        } else if (t.type == SatJobStreamProcessor::SatTask::SPLIT) {
            newLiterals.push_back(INT32_MAX);
            for (int a : assumptions) newLiterals.push_back(a);
            newLiterals.push_back(0);
            newLiterals.push_back(INT32_MIN);
        }

        auto nameOfCall = copy["name"].get<std::string>();
        StaticStore<std::vector<int>>::insert(nameOfCall, std::move(newLiterals));
        copy["internalliterals"] = nameOfCall;
        if (!descriptionLabel.empty()) {
            copy["description-id"] = descriptionLabel;
        }
        _expected_result_job_name = nameOfCall;

        LOG(V5_DEBG, "MSJS %s begin call\n", _name.c_str());
        _dtask->startActiveTime();
        _task_pending = true;
        _pending_rev = t.rev;
        _pending_task_interrupted = false;
        try {
            LOG(V4_VVER, "%s SUBMIT %s\n", _name.c_str(), nameOfCall.c_str());
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
                const int solSize = solution.size();
                bool winner = concludeRevision(rev, resultCode, std::move(solution));
                if (winner) {
                    _last_won_rev = rev;
                    LOG(V3_VERB, "%s rev. %i (internally %i) won with res=%i solsize=%i\n",
                        _name.c_str(), rev, subjob, resultCode, solSize);
                }
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
        while (continueWaitingForTask(t.rev)) {
            usleep(sleepInterval);
            sleepInterval = std::min(2500UL, (unsigned long) std::ceil(1.2*sleepInterval));
        }

        _dtask->commitActiveTime();
        LOG(V5_DEBG, "MSJS %s call ended\n", _name.c_str());

        _backlog_task = SatTask{_backlog_task.type};
    }

    void yield() {

        // already (in the process of being) finalized?
        if (_finalized || !_began_nontrivial_solving) return;

        LOG(V3_VERB, "%s yielding\n", _name.c_str());

        if (_dtask) _dtask->evicted = true; // mark as evicted yourself
        while (_task_pending) usleep(1000);
        if (!_incremental) return;
        if (!_json_base.contains("name")) return;
        _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        _json_base["name"] = _base_job_name + std::to_string(_subjob_counter++);
        nlohmann::json copy(_json_base);
        copy["done"] = true;
        // The callback is never called.
        LOG(V4_VVER, "%s closing API\n", _name.c_str());
        _api.submit(copy, [&](nlohmann::json& result) {assert(false);});
        LOG(V4_VVER, "%s closed API\n", _name.c_str());

        _began_nontrivial_solving = false;
        _retrieve_complete_task = true;
        _dtask = {};
        initJson();
    }

    virtual void finalize() override {
        yield();
        LOG(V4_VVER, "%s do finalize\n", _name.c_str());
        _finalized = true;
        SatJobStreamProcessor::finalize();
    }

    void setGroupId(const std::string& groupId, int minVar = -1, int maxVar = -1) {
        LOG(V2_INFO, "%s group ID %s V=[%i,%i]\n", _base_job_name.c_str(), groupId.c_str(), minVar, maxVar);
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
    bool continueWaitingForTask(int rev) {
        if (!_task_pending) return false;
        if (_pending_task_interrupted) return false; // do NOT wait for interrupted call to return
        assert(_dtask);
        if (!_terminator(rev) && !_dtask->evicted) return true;

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
        LOG(V4_VVER, "%s interrupt\n", _name.c_str());
        auto response = _api.submit(jsonInterrupt, [&](nlohmann::json& result) {assert(false);});
        if (response == JsonInterface::Result::DISCARD) {
            concludeRevision(_pending_rev, 0, {});
            _task_pending = false;
        }
        return false; // do NOT wait for interrupted call to return
    }
};
