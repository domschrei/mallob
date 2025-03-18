
#pragma once

#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/sat/data/revision_data.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "data/job_result.hpp"
#include "../job/sat_process_config.hpp"
#include "../solvers/portfolio_solver_interface.hpp"
#include "variable_translator.hpp"
#include "../parse/serialized_formula_parser.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "app/sat/execution/solver_setup.hpp"

// Forward declarations
class SatEngine;
class Logger;
class Parameters;
class SerializedFormulaParser;

class SolverThread {

private:
    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    Logger& _logger;
    std::thread _thread;

    std::vector<std::unique_ptr<SerializedFormulaParser>> _pending_formulae;
    std::vector<std::pair<size_t, const int*>> _pending_assumptions;

    SplitMix64Rng _rng;

    LratConnector* _lrat;

    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;
    int _local_solvers_count;
    long _tid = -1;

    Mutex _state_mutex;
    ConditionVariable _state_cond;

    std::atomic_int _latest_revision = 0;
    Checksum _latest_checksum;
    std::atomic_int _active_revision = 0;
    unsigned long _imported_lits_curr_revision = 0;
    bool _last_read_lit_zero = true;
    int _max_var = 0;
    Checksum _running_chksum;
    VariableTranslator _vt;
    bool _has_pseudoincremental_solvers;

    std::atomic_bool _initialized = false;
    std::atomic_bool _terminated = false;
    bool _in_solve_call = false;

    bool _found_result = false;
    JobResult _result;

public:
    SolverThread(const Parameters& params, const SatProcessConfig& config, std::shared_ptr<PortfolioSolverInterface> solver, 
                RevisionData firstRevision, int localId);
    ~SolverThread();

    void start();
    void appendRevision(int revision, RevisionData data);
    void setTerminate(bool cleanUpAsynchronously = false) {
        {
            auto lock = _state_mutex.getLock();
            if (_terminated) return;
            _terminated = true;
        }
        _state_cond.notify();
        _solver.setTerminate();
        // Clean up solver
        if (cleanUpAsynchronously) {
            while (_thread.joinable() && !_initialized) usleep(1000);
            _solver.cleanUp();
            // also asynchronously close LRAT pipelines
            if (_lrat) _lrat->stop();
            if (_solver.getSolverSetup().owningModelCheckingLratConnector) {
                _solver.getSolverSetup().modelCheckingLratConnector->stop();
            }
        }
    }
    void tryJoin() {if (_thread.joinable()) _thread.join();}

    bool isInitialized() const {
        return _initialized;
    }
    int getTid() const {
        return _tid;
    }
    bool hasFoundResult(int revision) {
        auto lock = _state_mutex.getLock();
        return _initialized && _active_revision == revision && _found_result;
    }
    JobResult& getSatResult() {
        return _result;
    }
    bool hasPreprocessedFormula() {
        return _solver.hasPreprocessedFormula();
    }
    std::vector<int>&& extractPreprocessedFormula() {
        return std::move(_solver.extractPreprocessedFormula());
    }

    int getActiveRevision() const {return _active_revision;}

private:
    void init();
    void* run();
    
    void pin();
    bool readFormula();

    void diversifyInitially();
    void diversifyAfterReading();

    void runOnce();
    
    void waitWhileSolved();
    void waitUntil(std::function<bool()> predicate);
    
    void reportResult(int res, int revision);

    const char* toStr();

};
