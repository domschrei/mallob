
#pragma once

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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
#include "util/sys/terminator.hpp"
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

    SatJobStream _job_stream;
    MallobSatJobStreamProcessor* _mallob_processor {nullptr};

    std::vector<int> _lits;
    std::vector<int> _assumptions;
    int _nb_vars {0};
    int _nb_clauses {0};
    int _revision {-1};

    bzla::Terminator* _terminator {nullptr};

    std::vector<int> _solution;
    tsl::robin_set<int> _failed_lits;

    float _start_time {0};
    bool _in_solved_state {false};
    bzla::Result _result;

public:
    BitwuzlaSatConnector(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& name) :
        bzla::sat::SatSolver(), _params(params), _desc(desc), _stream_id(getNextStreamId()),
        _name(name + ":" + std::to_string(_stream_id) + "(SAT)"), _job_stream(_name) {

        _mallob_processor = new MallobSatJobStreamProcessor(params, api, desc,
            _name, _stream_id, true, _job_stream.getSynchronizer());
        _job_stream.addProcessor(_mallob_processor);
        LOG(V2_INFO, "New: %s\n", _name.c_str());

        auto internalProcessor = new InternalSatJobStreamProcessor(true, _job_stream.getSynchronizer());
        _job_stream.addProcessor(internalProcessor);

        _start_time = Timer::elapsedSeconds();
    }
    virtual ~BitwuzlaSatConnector() {
        LOG(V2_INFO, "Done: %s\n", _name.c_str());
        _job_stream.interrupt();
        _job_stream.finalize();
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
        this->_terminator = terminator;
    }

    virtual bzla::Result solve() override {
        if (_in_solved_state) return _result;

        _job_stream.setTerminator([&]() {return isTimeoutHit();});

        _revision++;
        if (_revision == 0 && _mallob_processor) {
            _mallob_processor->setInitialSize(_nb_vars, _nb_clauses);
        }
        auto time = Timer::elapsedSeconds();
        LOG(V2_INFO, "%s submit rev. %i (%i lits)\n", _name.c_str(), _revision, _lits.size());

        auto [resultCode, solution] = _job_stream.solve(std::move(_lits), _assumptions);
        _lits.clear();
        _assumptions.clear();

        bzla::Result bzlaResult = bzla::Result::UNKNOWN;
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "%s rev. %i done - time=%.3fs res=%i\n", _name.c_str(), _revision, time, resultCode);
        if (resultCode == 10) bzlaResult = bzla::Result::SAT;
        if (resultCode == 20) bzlaResult = bzla::Result::UNSAT;

        if (bzlaResult == bzla::Result::SAT) {
            _solution = std::move(solution);
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

private:
    bool isTimeoutHit() const {
        if (_terminator && _terminator->terminate())
            return true;
        if (_params.timeLimit() > 0 && Timer::elapsedSeconds() >= _params.timeLimit())
            return true;
        if (_desc.getWallclockLimit() > 0 && (Timer::elapsedSeconds() - _start_time) >= _desc.getWallclockLimit())
            return true;
        if (Terminator::isTerminating())
            return true;
        return false;
    }
};
