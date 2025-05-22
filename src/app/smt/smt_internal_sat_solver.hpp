
#pragma once

#include <cstdint>
#include <string>
#include <unistd.h>

#include "app/maxsat/sat_job_stream.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "bitwuzla/cpp/sat_solver.h"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "robin_set.h"
#include "util/logger.hpp"
#include "util/sys/terminator.hpp"

class SmtInternalSatSolver : public bzla::sat::SatSolver {

private:
    const Parameters& _params;
    JobDescription& _desc;

    static int getNextStreamId() {
        static int _stream_id = 1;
        return _stream_id++;
    }
    static std::string getNextUserName() {
        static int nameCounter = 1;
        return "__smt-u" + std::to_string(nameCounter++);
    }
    SatJobStream _job;

    std::vector<int> _lits;
    std::vector<int> _assumptions;
    int _nb_vars {0};
    int _nb_clauses {0};

    bzla::Terminator* _terminator {nullptr};

    std::vector<int> _solution;
    tsl::robin_set<int> _failed_lits;

    float _start_time {0};

public:
    SmtInternalSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        bzla::sat::SatSolver(), _params(params), _desc(desc),
        _job(params, api, desc, getNextUserName(), getNextStreamId(), true) {
        LOG(V2_INFO, "Create SMT-internal SAT solver %s\n", _job.getUserName().c_str());
        _start_time = Timer::elapsedSeconds();
    }
    virtual ~SmtInternalSatSolver() {
        LOG(V2_INFO, "Delete SMT-internal SAT solver %s\n", _job.getUserName().c_str());
        _job.interrupt();
        _job.finalize();
    }

    virtual const char* get_name() const override {return "MallobSat-internal";}
    virtual const char* get_version() const override {return "N/A";}

    virtual void add(int32_t lit) override {
        _lits.push_back(lit);
        _nb_vars = std::max(_nb_vars, std::abs(lit));
        _nb_clauses += lit == 0;
    }
    virtual void assume(int32_t lit) override {
        _assumptions.push_back(lit);
    }

    virtual void configure_terminator(bzla::Terminator* terminator) override {
        this->_terminator = terminator;
    }

    virtual bzla::Result solve() override {

        if (_job.getRevision() == 0) {
            // Specify # variables and # clauses only for first increment of the job
            _desc.getAppConfiguration().updateFixedSizeEntry("__NV", _nb_vars);
            _desc.getAppConfiguration().updateFixedSizeEntry("__NC", _nb_clauses);
        }

        LOG(V2_INFO, "SMT %s submit rev. %i (%i total cls)\n", _job.getUserName().c_str(), _job.getRevision(), _nb_clauses);
        _job.submitNext(std::move(_lits), _assumptions);
        _lits.clear();
        _assumptions.clear();

        bool interrupted = false;
        unsigned long sleepInterval {1};
        while (_job.isPending()) {
            usleep(sleepInterval);
            if (isTimeoutHit()) {
                _job.interrupt();
                interrupted = true;
            }
            sleepInterval = std::max(2500UL, 2*sleepInterval);
        }

        bzla::Result bzlaResult = bzla::Result::UNKNOWN;
        nlohmann::json result;
        if (interrupted || _job.isRejected()) {
            LOG(V2_INFO, "SMT %s interrupted / rejected\n", _job.getUserName().c_str());
        } else {
            result = std::move(_job.getResult());
            int resultCode = result["result"]["resultcode"];
            LOG(V2_INFO, "SMT %s done, res=%i\n", _job.getUserName().c_str(), resultCode);
            if (resultCode == 10) bzlaResult = bzla::Result::SAT;
            if (resultCode == 20) bzlaResult = bzla::Result::UNSAT;
        }

        if (bzlaResult == bzla::Result::SAT) {
            if (_params.compressModels()) {
                _solution = ModelStringCompressor::decompress(result["result"]["solution"].get<std::string>());
            } else {
                _solution = result["result"]["solution"].get<std::vector<int>>();
            }
        }
        if (bzlaResult == bzla::Result::UNSAT) {
            _failed_lits.clear();
            for (int lit : result["result"]["solution"].get<std::vector<int>>()) {
                _failed_lits.insert(lit);
            }
        }

        return bzlaResult;
    }

    virtual int32_t value(int32_t lit) override {
        int var = std::abs(lit);
        assert(var < _solution.size());
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
        //assert(false);
        return 0;
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
