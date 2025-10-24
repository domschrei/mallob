
#pragma once

#include <algorithm>
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

#include "app/incsat/inc_sat_controller.hpp"
#include "bitwuzla/cpp/sat_solver.h"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "robin_set.h"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/timer.hpp"

class BitwuzlaSatConnector : public bitwuzla::SatSolver {

private:
    const Parameters& _params;
    JobDescription& _desc;

    std::string _name;

    std::vector<int> _lits;
    std::vector<int> _assumptions;
    int _nb_vars {0};
    int _nb_clauses {0};
    int _revision {-1};

    std::vector<int> _solution;
    tsl::robin_set<int> _failed_lits;

    bool _in_solved_state {false}; // whether a result would already be known for an immediate solve() call
    bitwuzla::Result _result;

    std::ostream* _out_stream {nullptr};

    std::unique_ptr<IncSatController> _incsat;

    std::function<void()> _cb_cleanup;
    bitwuzla::Terminator* _bzla_term {nullptr};

public:
    BitwuzlaSatConnector(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker, const std::string& name) :
        bitwuzla::SatSolver(), _params(params), _desc(desc),
        _name(name) {

        _incsat.reset(new IncSatController(_params, api, _desc, tracker));
        _incsat->setInnerTerminator([&]() {
            return _bzla_term && _bzla_term->terminate();
        });
    }
    virtual ~BitwuzlaSatConnector() {
        LOG(V2_INFO, "Done: %s\n", _name.c_str());
        if (_cb_cleanup) _cb_cleanup();
    }

    void setCleanupCallback(std::function<void()> cb) {_cb_cleanup = cb;}
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

    virtual void configure_terminator(bitwuzla::Terminator* terminator) override {
        if (_bzla_term) LOG(V1_WARN, "[WARN] overriding bzla terminator\n");
        _bzla_term = terminator;
    }

    virtual bitwuzla::Result solve() override {
        if (_in_solved_state) return _result;

        _revision++;
        auto time = Timer::elapsedSeconds();
        LOG(V2_INFO, "%s submit rev. %i (%i lits, %i asmpt)\n", _name.c_str(), _revision, _lits.size(), _assumptions.size());

        bool noAssumptions = _assumptions.empty();
        auto [resultCode, solution] = _incsat->solveNextRevision(std::move(_lits), std::move(_assumptions));
        _in_solved_state = noAssumptions;
        _lits.clear();
        _assumptions.clear();

        bitwuzla::Result bzlaResult = bitwuzla::Result::UNKNOWN;
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "%s rev. %i done - time=%.3fs res=%i\n", _name.c_str(), _revision, time, resultCode);
        if (resultCode == 10) bzlaResult = bitwuzla::Result::SAT;
        if (resultCode == 20) bzlaResult = bitwuzla::Result::UNSAT;

        if (bzlaResult == bitwuzla::Result::SAT) {
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
        if (bzlaResult == bitwuzla::Result::UNSAT) {
            _failed_lits.clear();
            for (int lit : solution) _failed_lits.insert(lit);
        }

        _result = bzlaResult;
        return _result;
    }

    virtual int32_t value(int32_t lit) override {
        int var = std::abs(lit);
        if (var >= _solution.size()) {
            LOG(V1_WARN, "[WARN] Solution has size %lu - variable %i queried!\n", _solution.size(), var);
            return 0;
        }
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
};
