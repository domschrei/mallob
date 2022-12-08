
#pragma once

#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <thread>
#include <atomic>
#include <list>

#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "data/job_result.hpp"
#include "../job/sat_process_config.hpp"
#include "../solvers/portfolio_solver_interface.hpp"
#include "solving_state.hpp"
#include "clause_shuffler.hpp"
#include "variable_translator.hpp"
#include "../parse/serialized_formula_parser.hpp"

// Forward declarations
class SatEngine;

class SolverThread {

private:
    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    Logger& _logger;
    std::thread _thread;

    std::vector<SerializedFormulaParser> _pending_formulae;
    std::vector<std::pair<size_t, const int*>> _pending_assumptions;

    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;
    int _local_solvers_count;
    long _tid = -1;

    Mutex _state_mutex;
    ConditionVariable _state_cond;

    std::atomic_int _latest_revision = 0;
    std::atomic_int _active_revision = 0;
    unsigned long _imported_lits_curr_revision = 0;
    bool _last_read_lit_zero = true;
    int _max_var = 0;
    VariableTranslator _vt;
    bool _has_pseudoincremental_solvers;

    std::atomic_bool _initialized = false;
    std::atomic_bool _interrupted = false;
    std::atomic_bool _suspended = false;
    std::atomic_bool _terminated = false;

    bool _found_result = false;
    JobResult _result;


public:
    SolverThread(const Parameters& params, const SatProcessConfig& config, std::shared_ptr<PortfolioSolverInterface> solver, 
                size_t fSize, const int* fLits, size_t aSize, const int* aLits, int localId);
    ~SolverThread();

    void start();
    void appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits);
    void setSuspend(bool suspend) {
        {
            auto lock = _state_mutex.getLock();
            _suspended = suspend;
            if (_suspended) _solver.suspend();
            else _solver.resume();
        }
        _state_cond.notify();
    }
    void setTerminate() {
        _solver.setTerminate();
        _terminated = true;
        _state_cond.notify();
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
    void waitWhileSuspended();
    void waitUntil(std::function<bool()> predicate);
    
    void reportResult(int res, int revision);

    const char* toStr();

};
