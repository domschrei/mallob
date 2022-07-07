
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

// Forward declarations
class SatEngine;

class SolverThread {

private:
    struct Task {

        int revision;
        size_t numLiterals;
        const int* literals;
        size_t numAssumptions;
        const int* assumptions;
        size_t numReadLiterals = 0;
        
        bool attemptToSolve = true;
        bool hasResultToPublish = false;
        JobResult result;

        Task() = default;
        Task(const Task& other) = default;
        Task(int revision, size_t numLits, const int* lits, size_t numAsmpt, const int* asmpt) :
            revision(revision), numLiterals(numLits), literals(lits),
            numAssumptions(numAsmpt), assumptions(asmpt) {

            result.result = UNKNOWN;
        }
        Task(Task&& moved) :
            revision(moved.revision), numLiterals(moved.numLiterals), literals(moved.literals),
            numAssumptions(moved.numAssumptions), assumptions(moved.assumptions), 
            result(std::move(moved.result)) {}
    };

    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    Logger& _logger;
    std::thread _thread;

    // Queue of pending increments
    Mutex _state_mutex;
    ConditionVariable _state_cond;
    std::vector<std::unique_ptr<Task>> _tasks;

    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;
    int _local_solvers_count;
    long _tid = -1;

    bool _last_read_lit_zero = true;
    int _max_var = 0;
    VariableTranslator _vt;

    bool _has_pseudoincremental_solvers;

    std::atomic_bool _initialized = false;
    std::atomic_bool _interrupted = false;
    std::atomic_bool _suspended = false;
    std::atomic_bool _terminated = false;

public:
    SolverThread(const Parameters& params, const SatProcessConfig& config, std::shared_ptr<PortfolioSolverInterface> solver, 
                size_t fSize, const int* fLits, size_t aSize, const int* aLits, int localId,
                bool attemptToSolve1stRev);
    ~SolverThread();

    void start();
    void appendTask(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits, bool attemptToSolve);
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
        {
            auto lock = _state_mutex.getLock();
            _solver.setTerminate();
            _terminated = true;
        }
        _state_cond.notify();
    }
    void tryJoin() {if (_thread.joinable()) _thread.join();}

    bool isInitialized() const {
        return _initialized;
    }
    int getTid() const {
        return _tid;
    }
    int getRevisionWithReadyResult() {
        auto lock = _state_mutex.getLock();
        for (auto& task : _tasks) {
            if (task->hasResultToPublish) return task->revision;
        }
        return -1;
    }
    JobResult& getSatResult(int revision) {
        auto& task = getTask(revision);
        assert(task.hasResultToPublish);
        task.hasResultToPublish = false;
        return task.result;
    }

private:
    void init();
    void* run();
    
    void pin();

    void diversifyInitially();
    void diversifyAfterReading();

    void readFormula(Task& task);
    void processTask(Task& task);
    
    void waitWhileSuspended();
    void waitUntil(std::function<bool()> predicate);
    
    void extractResult(int res, int revision, Task& task);

    Task& getTask(int revision) {
        auto lock = _state_mutex.getLock();
        return *_tasks.at(revision);
    }
    const char* toStr();

};
