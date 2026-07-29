
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

    bool _has_empty_clause {false};
    bool _last_lit_zero {true};

    std::vector<int> _solution;
    tsl::robin_set<int> _failed_lits;

    bool _in_solved_state {false}; // whether a result would already be known for an immediate solve() call
    bitwuzla::Result _result;

    std::ostream* _out_stream {nullptr};

    std::unique_ptr<IncSatController> _incsat;

    std::function<void()> _cb_cleanup;
    bitwuzla::Terminator* _bzla_term {nullptr};
    bitwuzla::Terminator* _ext_term {nullptr};

    std::function<void(std::unique_ptr<IncSatController>&&)> _incsat_cleaner;

public:
    BitwuzlaSatConnector(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker, const std::string& name) :
        bitwuzla::SatSolver(), _params(params), _desc(desc),
        _name(name) {

        _incsat.reset(new IncSatController(_params, api, _desc, tracker));
        _incsat->setInnerTerminator([&]() {
            if (_bzla_term && _bzla_term->terminate()) return true;
            if (_ext_term && _ext_term->terminate()) return true;
            return false;
        }, true);
    }
    virtual ~BitwuzlaSatConnector() {
        LOG(V2_INFO, "Done: %s\n", _name.c_str());
        if (_cb_cleanup) _cb_cleanup();
        if (_incsat_cleaner) {
            // Important: We need to stop internal access to the terminators
            // in a thread-safe way before cleaning up the terminators!
            _incsat->invalidateTerminators();
            _incsat_cleaner(std::move(_incsat));
        }
    }

    void preInitialize() {
        _incsat->initInteractiveSolving();
    }

    void setCleanupCallback(std::function<void()> cb) {_cb_cleanup = cb;}
    void outputModels(std::ostream* os) {
        _out_stream = os;
    }

    void setIncSatCleaner(std::function<void(std::unique_ptr<IncSatController>&&)> cleaner) {
        _incsat_cleaner = cleaner;
    }

    virtual const char* get_name() const override {return "MallobSat-internal";}
    virtual const char* get_version() const override {return "N/A";}

    virtual void add(int32_t lit, int64_t cgroup_id = 0) override {
        _in_solved_state = false;
        const bool isZero = lit == 0;
        if (MALLOB_UNLIKELY(isZero && _last_lit_zero))
            _has_empty_clause = true;
        _last_lit_zero = isZero;
        _lits.push_back(lit);
        _nb_vars = std::max(_nb_vars, std::abs(lit));
        _nb_clauses += isZero;
    }
    virtual void assume(int32_t lit) override {
        _in_solved_state = false;
        _assumptions.push_back(lit);
    }

    virtual void configure_terminator(bitwuzla::Terminator* terminator) override {
        _bzla_term = terminator;
    }
    void configure_ext_terminator(bitwuzla::Terminator* terminator) {
        _ext_term = terminator;
    }

    inline bitwuzla::Result returnFromSolve() {
        LOG(V6_DEBGV, "RETURN_SOLVE code=%i\n", _result);
        return _result;
    }

    virtual bitwuzla::Result solve() override {

        if (_in_solved_state) {
            return returnFromSolve();
        }

        if (_nb_clauses == 0 && _lits.empty() && _assumptions.empty()) {
            _failed_lits.clear();
            _result = bitwuzla::Result::SAT;
            _solution = {0};
            _in_solved_state = true;
            return returnFromSolve();
        }

        if (_has_empty_clause) {
            // Empty clause is part of the permanent clauses:
            // Enter a "solved" state with UNSAT and no failed assumptions
            LOG(V2_INFO, "%s trivially UNSAT\n", _name.c_str());
            _lits.clear();
            _assumptions.clear();
            _failed_lits.clear();
            _result = bitwuzla::Result::UNSAT;
            _in_solved_state = true;
            return returnFromSolve();
        }

        _revision++;
        auto time = Timer::elapsedSeconds();
        LOG(V2_INFO, "%s submit rev. %i (%i lits, %i asmpt)\n", _name.c_str(), _revision, _lits.size(), _assumptions.size());

        std::vector<int> assumptionsToCheck = _params.bitwuzlaCheckSatModels() ? _assumptions : std::vector<int>();

        bool noAssumptions = _assumptions.empty();
        auto [resultCode, solution] = _incsat->solveNextRevision(std::move(_lits), std::move(_assumptions),
            _nb_vars, _nb_clauses);
        _in_solved_state = noAssumptions;

        if (resultCode == 10 && _params.bitwuzlaCheckSatModels()) {
            // check model
            bool clauseSat = false;
            for (int lit : _lits) {
                if (lit == 0) {
                    if (!clauseSat) {
                        LOG(V0_CRIT, "[ERROR] Returned model does not satisfy formula!\n");
                        abort();
                    }
                    clauseSat = false;
                    continue;
                }
                auto value = solution[std::abs(lit)];
                assert(value == lit || value == -lit);
                clauseSat |= (value == lit);
            }
            // check assumptions
            for (int lit : assumptionsToCheck) {
                auto value = solution[std::abs(lit)];
                assert(value == lit || value == -lit);
                if (value != lit) {
                    LOG(V0_CRIT, "[ERROR] Returned model does not satisfy assumption %i!\n", lit);
                    abort();
                }
            }
        }

        _lits.clear();
        _assumptions.clear();

        bitwuzla::Result bzlaResult = bitwuzla::Result::UNKNOWN;
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "%s rev. %i done - time=%.3fs res=%i slen=%lu\n", _name.c_str(), _revision, time, resultCode,
            solution.size());
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
        return returnFromSolve();
    }

    virtual int32_t value(int32_t lit) override {
        int var = std::abs(lit);
        if (var >= _solution.size()) {
            LOG(V1_WARN, "[WARN] Solution has size %lu - variable %i queried!\n", _solution.size(), var);
            assert(false);
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
