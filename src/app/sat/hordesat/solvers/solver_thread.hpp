
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

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
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "app/sat/hordesat/solvers/solving_state.hpp"
#include "app/sat/hordesat/utilities/clause_shuffler.hpp"
#include "app/sat/hordesat/utilities/variable_translator.hpp"

// Forward declarations
class HordeLib;

class SolverThread {

private:
    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    Logger& _logger;
    std::thread _thread;

    std::vector<std::pair<size_t, const int*>> _pending_formulae;
    std::vector<std::pair<size_t, const int*>> _pending_assumptions;
    
    ClauseShuffler _shuffler;
    bool _shuffle;

    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;
    long _tid = -1;

    Mutex _state_mutex;
    ConditionVariable _state_cond;

    std::atomic_int _latest_revision = 0;
    std::atomic_int _active_revision;
    std::atomic_ulong _imported_lits_curr_revision = 0;
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
    SolverThread(const Parameters& params, std::shared_ptr<PortfolioSolverInterface> solver, 
                size_t fSize, const int* fLits, size_t aSize, const int* aLits, int localId);
    ~SolverThread();

    void start();
    void appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits);
    void setInterrupt(bool interrupt) {
        {
            auto lock = _state_mutex.getLock();
            _interrupted = interrupt;
            if (_interrupted) _solver.interrupt();
        }
        _state_cond.notify();
    }
    void setSuspend(bool suspend) {
        {
            auto lock = _state_mutex.getLock();
            _suspended = suspend;
            if (_suspended) _solver.suspend();
        }
        _state_cond.notify();
    }
    void setTerminate() {_terminated = true;};
    void tryJoin() {if (_thread.joinable()) _thread.join();}

    bool isInitialized() const {
        return _initialized;
    }
    int getTid() const {
        return _tid;
    }
    bool hasFoundResult(int revision) {
        auto lock = _state_mutex.getLock();
        return _active_revision == revision && _found_result;
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
    void waitWhileInterrupted();
    void waitWhileSuspended();
    void waitUntil(std::function<bool()> predicate);
    
    void reportResult(int res);

    const char* toStr();

};

#endif