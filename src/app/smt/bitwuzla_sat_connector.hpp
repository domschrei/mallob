
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <ostream>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/sat/stream/internal_sat_job_stream_processor.hpp"
#include "app/sat/stream/mallob_sat_job_stream_processor.hpp"
#include "app/sat/stream/sat_job_stream.hpp"
#include "bitwuzla/cpp/sat_solver.h"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "robin_set.h"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"

class BitwuzlaSatConnector : public bzla::sat::SatSolver {

private:
    const Parameters& _params;
    JobDescription& _desc;

    static int getNextStreamId() {
        static int _stream_id = 1;
        return _stream_id++;
    }
    int _stream_id;
    std::string _name;

    std::vector<int> _lits;
    std::vector<int> _assumptions;
    int _nb_vars {0};
    int _nb_clauses {0};
    int _revision {-1};

    std::vector<int> _solution;
    tsl::robin_set<int> _failed_lits;

    float _start_time {0};
    bool _in_solved_state {false};
    bzla::Result _result;

    std::ostream* _out_stream {nullptr};

    struct WrappedSatJobStream {
        SatJobStream stream;
        MallobSatJobStreamProcessor* mallobProcessor {nullptr};
        std::atomic<bzla::Terminator*> bzlaTerminator {nullptr};
        WrappedSatJobStream(const std::string& name) : stream(name) {}
    };
    std::unique_ptr<WrappedSatJobStream> _stream_wrapper;

    struct GarbageCollector {
        Mutex mtxGarbage;
        ConditionVariable condVarGarbage;
        std::list<std::unique_ptr<WrappedSatJobStream>> garbage;
        volatile bool stop {false};
        std::thread bgThread;
        GarbageCollector() {
            bgThread = std::thread([this]() {run();});
        }
        void run() {
            while (true) {
                std::unique_ptr<WrappedSatJobStream> item;
                {
                    auto lock = mtxGarbage.getLock();
                    condVarGarbage.waitWithLockedMutex(lock, [&]() {
                        return !garbage.empty() || stop;
                    });
                    if (garbage.empty()) break;
                    item = std::move(garbage.front());
                    garbage.pop_front();
                }
                item.reset(); // delete garbage
            }
        }
        void add(std::unique_ptr<WrappedSatJobStream>&& item) {
            {
                auto lock = mtxGarbage.getLock();
                garbage.push_back(std::move(item));
            }
            condVarGarbage.notify();
        }
        ~GarbageCollector() {
            stop = true;
            add({});
            bgThread.join();
        }
    };
    static GarbageCollector* _gc;

public:
    BitwuzlaSatConnector(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& name) :
        bzla::sat::SatSolver(), _params(params), _desc(desc), _stream_id(getNextStreamId()),
        _name(name + ":" + std::to_string(_stream_id) + "(SAT)") {

        _stream_wrapper.reset(new WrappedSatJobStream(_name));

        _stream_wrapper->mallobProcessor = new MallobSatJobStreamProcessor(params, api, desc,
            _name, _stream_id, true, _stream_wrapper->stream.getSynchronizer());
        _stream_wrapper->stream.addProcessor(_stream_wrapper->mallobProcessor);
        LOG(V2_INFO, "New: %s\n", _name.c_str());

        auto internalProcessor = new InternalSatJobStreamProcessor(true, _stream_wrapper->stream.getSynchronizer());
        _stream_wrapper->stream.addProcessor(internalProcessor);

        _stream_wrapper->stream.setTerminator([&, wrapper=_stream_wrapper.get(), params=&_params, desc=&_desc, startTime=_start_time]() {
            if (wrapper->stream.finalizing()) return true;
            auto bzlaTerm = wrapper->bzlaTerminator.load(std::memory_order_relaxed);
            if (bzlaTerm && bzlaTerm->terminate()) return true;
            return isTimeoutHit(params, desc, startTime);
        });

        _start_time = Timer::elapsedSeconds();
    }
    virtual ~BitwuzlaSatConnector() {
        LOG(V2_INFO, "Done: %s\n", _name.c_str());
        _gc->add(std::move(_stream_wrapper));
    }

    void outputModels(std::ostream* os) {
        _out_stream = os;
    }

    virtual const char* get_name() const override {return "MallobSat-internal";}
    virtual const char* get_version() const override {return "N/A";}

    virtual void add(int32_t lit) override {
        _in_solved_state = false;
        _lits.push_back(lit);
        _nb_vars = std::max(_nb_vars, std::abs(lit));
        _nb_clauses += lit == 0;
    }
    virtual void assume(int32_t lit) override {
        _in_solved_state = false;
        _assumptions.push_back(lit);
    }

    virtual void configure_terminator(bzla::Terminator* terminator) override {
        _stream_wrapper->bzlaTerminator.store(terminator, std::memory_order_relaxed);
    }

    virtual bzla::Result solve() override {
        if (_in_solved_state) return _result;

        _revision++;
        if (_revision == 0 && _stream_wrapper->mallobProcessor) {
            _stream_wrapper->mallobProcessor->setInitialSize(_nb_vars, _nb_clauses);
        }
        auto time = Timer::elapsedSeconds();
        LOG(V2_INFO, "%s submit rev. %i (%i lits)\n", _name.c_str(), _revision, _lits.size());

        auto [resultCode, solution] = _stream_wrapper->stream.solve(std::move(_lits), _assumptions);
        _lits.clear();
        _assumptions.clear();

        bzla::Result bzlaResult = bzla::Result::UNKNOWN;
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "%s rev. %i done - time=%.3fs res=%i\n", _name.c_str(), _revision, time, resultCode);
        if (resultCode == 10) bzlaResult = bzla::Result::SAT;
        if (resultCode == 20) bzlaResult = bzla::Result::UNSAT;

        if (bzlaResult == bzla::Result::SAT) {
            _solution = std::move(solution);
            if (_out_stream) {
                *_out_stream << _name << " : MODEL " << _revision << " : ";
                for (int v = 1; v < _solution.size(); v++) {
                    assert(std::abs(_solution[v]) == v);
                    *_out_stream << (_solution[v] > 0 ? 1 : 0);
                }
                *_out_stream << std::endl;
            }
        }
        if (bzlaResult == bzla::Result::UNSAT) {
            _failed_lits.clear();
            for (int lit : solution) _failed_lits.insert(lit);
        }

        _in_solved_state = true;
        _result = bzlaResult;
        return _result;
    }

    virtual int32_t value(int32_t lit) override {
        int var = std::abs(lit);
        assert(var < _solution.size() || log_return_false("[ERROR] Solution has size %lu - variable %i queried!\n", _solution.size(), var));
        int val = _solution[var];
        assert(std::abs(val) == var);
        if (val > 0) return 1;
        if (val < 0) return -1;
        return 0;
    }
    virtual bool failed(int32_t lit) override {
        assert(!_failed_lits.count(-lit));
        return _failed_lits.count(lit);
    }
    virtual int32_t fixed(int32_t lit) override {
        // TODO
        return 0; // -1: not implied, 1: implied, 0: unknown
    }

    bool isTimeoutHit(const Parameters* params, JobDescription* desc, float startTime) const {
        if (Terminator::isTerminating())
            return true;
        if (params->timeLimit() > 0 && Timer::elapsedSeconds() >= params->timeLimit())
            return true;
        if (desc->getWallclockLimit() > 0 && (Timer::elapsedSeconds() - startTime) >= desc->getWallclockLimit())
            return true;
        return false;
    }
};
