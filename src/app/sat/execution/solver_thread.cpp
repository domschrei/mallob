
#include <sys/resource.h>
#include "util/assert.hpp"

#include "solver_thread.hpp"
#include "engine.hpp"
#include "util/sys/proc.hpp"
#include "util/hashing.hpp"

using namespace SolvingStates;

SolverThread::SolverThread(const Parameters& params, const SatProcessConfig& config,
         std::shared_ptr<PortfolioSolverInterface> solver, 
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        int localId, bool attemptToSolve1stRev) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(_solver.getLogger()), _local_id(localId), 
    _has_pseudoincremental_solvers(solver->getSolverSetup().hasPseudoincrementalSolvers) {
    
    _portfolio_rank = config.apprank;
    _portfolio_size = config.mpisize;
    _local_solvers_count = config.threads;

    appendTask(0, fSize, fLits, aSize, aLits, /*attemptToSolve=*/attemptToSolve1stRev);
}

void SolverThread::start() {
    if (!_thread.joinable()) _thread = std::thread([this]() {
        init();
        run();
    });
}

void SolverThread::init() {
    _tid = Proc::getTid();
    LOGGER(_logger, V5_DEBG, "tid %ld\n", _tid);
    std::string threadName = "SATSolver#" + std::to_string(_local_id);
    Proc::nameThisThread(threadName.c_str());
    _initialized = true;
}

void* SolverThread::run() {

    diversifyInitially();        
    int activeRevision = -1;

    while (!_terminated) {

        // Wait until the next task is available to process
        {
            auto lock = _state_mutex.getLock();
            _state_cond.waitWithLockedMutex(lock, [&]() {
                return _terminated || activeRevision+1 < _tasks.size();
            });
        }
        if (_terminated) break;

        // Proceed with next task
        activeRevision++;
        Task& task = getTask(activeRevision);
        
        // Read formula (suspending in between as necessary)
        readFormula(task);
        if (_terminated) break;
        
        // Diversify solver which may depend on input
        diversifyAfterReading();

        // Solve
        processTask(task);
    }

    LOGGER(_logger, V4_VVER, "exiting\n");
    return NULL;
}

void SolverThread::readFormula(Task& task) {
    constexpr int batchSize = 100000;

    // Fetch the task description to read
    size_t fSize = task.numLiterals;
    const int* fLits = task.literals;
    size_t aSize = task.numAssumptions;
    const int* aLits = task.assumptions;

    LOGGER(_logger, V4_VVER, "Reading rev. %i, start %i\n", (int)task.revision, (int)task.numReadLiterals);
    
    // Read the formula in batches from the point where you left off
    for (size_t start = task.numReadLiterals; start < fSize; start += batchSize) {

        size_t end = std::min(start+batchSize, fSize);
        for (size_t i = start; i < end; i++) {
            int lit = fLits[i];
            if (std::abs(lit) > 134217723) {
                LOGGER(_logger, V0_CRIT, "[ERROR] Invalid literal at rev. %i pos. %ld/%ld. Last %i literals: %i %i %i %i %i\n", 
                    (int)task.revision, i, fSize,
                    (int) std::min(i+1, (size_t)5),
                    i >= 4 ? fLits[i-4] : 0,
                    i >= 3 ? fLits[i-3] : 0,
                    i >= 2 ? fLits[i-2] : 0,
                    i >= 1 ? fLits[i-1] : 0,
                    lit
                );
                abort();
            }
            if (lit == 0 && _last_read_lit_zero) {
                LOGGER(_logger, V0_CRIT, "[ERROR] Empty clause at rev. %i pos. %ld/%ld. Last %i literals: %i %i %i %i %i\n", 
                    (int)task.revision, i, fSize,
                    (int) std::min(i+1, (size_t)5),
                    i >= 4 ? fLits[i-4] : 0,
                    i >= 3 ? fLits[i-3] : 0,
                    i >= 2 ? fLits[i-2] : 0,
                    i >= 1 ? fLits[i-1] : 0,
                    lit
                );
                abort();
            }
            _solver.addLiteral(_vt.getTldLit(lit));
            _max_var = std::max(_max_var, std::abs(lit));
            _last_read_lit_zero = lit == 0;
            task.numReadLiterals++;
            //_dbg_lits += std::to_string(lit]) + " ";
            //if (lit == 0) _dbg_lits += "\n";
        }
        
        waitWhileSuspended();
        if (_terminated) return;
    }
    for (size_t i = 0; i < aSize; i++) _max_var = std::max(_max_var, std::abs(aLits[i]));

    auto lock = _state_mutex.getLock();
    assert(task.numReadLiterals == fSize);

    // If necessary, introduce extra variable to the problem
    // to encode equivalence to the set of assumptions
    if (_has_pseudoincremental_solvers && task.revision >= _vt.getExtraVariables().size()) {
        _vt.addExtraVariable(_max_var);
        int aEquivVar = _vt.getExtraVariables().back();
        LOGGER(_logger, V4_VVER, "Encoding equivalence for %i assumptions of rev. %i @ var. %i\n", 
            aSize, (int)task.revision, (int)aEquivVar);
        
        // Make clause exporter append this condition
        // to each clause this solver exports
        if (_solver.exportsConditionalClauses() || !_solver.getSolverSetup().doIncrementalSolving)
            _solver.setCurrentCondVarOrZero(aSize > 0 ? aEquivVar : 0);
        
        if (aSize > 0) {
            // If all assumptions hold, then aEquivVar holds
            for (size_t i = 0; i < aSize; i++) {
                _solver.addLiteral(-_vt.getTldLit(aLits[i]));
            }
            _solver.addLiteral(aEquivVar);
            _solver.addLiteral(0);

            // If aEquivVar holds, then each assumption holds
            for (size_t i = 0; i < aSize; i++) {
                _solver.addLiteral(-aEquivVar);
                _solver.addLiteral(_vt.getTldLit(aLits[i]));
                _solver.addLiteral(0);
            }
        }
    }
}

void SolverThread::appendTask(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits, bool attemptToSolve) {
    {
        auto lock = _state_mutex.getLock();
        _tasks.emplace_back(new Task(revision, fSize, fLits, aSize, aLits));
        _tasks.back()->attemptToSolve = attemptToSolve;
        LOGGER(_logger, V4_VVER, "Received %i literals, %i assumptions\n", fSize, aSize);
        assert(revision+1 == (int)_tasks.size() || LOG_RETURN_FALSE("%i != %i", revision+1, _tasks.size()));
        //if (revision > 0) {
        //    _solver.interrupt();
        //    _interrupted = true;
        //}
    }
    _state_cond.notify();
}

void SolverThread::diversifyInitially() {

    // Random seed
    size_t seed = 42;
    hash_combine(seed, (unsigned int)_tid);
    hash_combine(seed, (unsigned int)_portfolio_size);
    hash_combine(seed, (unsigned int)_portfolio_rank);
    srand(seed);
    _solver.diversify(seed);
}

void SolverThread::diversifyAfterReading() {
	if (_solver.getGlobalId() >= _solver.getNumOriginalDiversifications()) {
        int solversCount = _local_solvers_count;
        int totalSolvers = solversCount * _portfolio_size;
        int vars = _solver.getVariablesCount();

        for (int var = 1; var <= vars; var++) {
            if (rand() % totalSolvers == 0) {
                _solver.setPhase(var, rand() % 2 == 1);
            }
        }
    }
}



void SolverThread::processTask(Task& task) {

    if (!task.attemptToSolve) return;

    // Set up correct solver state (or return if not possible)
    size_t aSize = task.numAssumptions;
    const int* aLits = task.assumptions;
    int revision = task.revision;
    {
        auto lock = _state_mutex.getLock();

        // Last point where upcoming solving attempt may be cancelled prematurely
        if (_suspended) return;

        // Make solver ready to solve by removing suspend flag
        _solver.setCurrentRevision(task.revision);
        _solver.resume();
    }

    // If necessary, translate assumption literals
    std::vector<int> tldAssumptions;
    if (!_vt.getExtraVariables().empty()) {
        for (size_t i = 0; i < aSize; i++) {
            tldAssumptions.push_back(_vt.getTldLit(aLits[i]));
        }
        aLits = tldAssumptions.data();
    }

    // Perform solving (blocking)
    LOGGER(_logger, V4_VVER, "BEGSOL rev. %i (%i assumptions)\n", revision, aSize);
    //std::ofstream ofs("DBG_" + std::to_string(_solver.getGlobalId()) + "_" + std::to_string(_active_revision));
    //ofs << _dbg_lits << "\n";
    //ofs.close();
    SatResult res;
    if (_solver.supportsIncrementalSat()) {
        res = _solver.solve(aSize, aLits);
    } else {
        // Add assumptions as permanent unit clauses
        for (const int* aLit = aLits; aLit != aLits+aSize; aLit++) {
            _solver.addLiteral(*aLit);
            _solver.addLiteral(0);
        }
        res = _solver.solve(0, nullptr);
    }
    // Uninterrupt solver (if it was interrupted)
    {
        auto lock = _state_mutex.getLock();
        _solver.uninterrupt();
        _interrupted = false;
    }
    LOGGER(_logger, V4_VVER, "ENDSOL\n");

    // Extract result and write into task, if applicable
    extractResult(res, revision, task);
}

void SolverThread::waitWhileSuspended() {
    waitUntil([&]{return _terminated || !_suspended;});
}
void SolverThread::waitUntil(std::function<bool()> predicate) {
    if (predicate()) return;
    _state_cond.wait(_state_mutex, predicate);
}

void SolverThread::extractResult(int res, int revision, Task& task) {

    if (res == 0 || task.hasResultToPublish) return;
    const char* resultString = res==SAT?"SAT":"UNSAT";

    auto lock = _state_mutex.getLock();

    //if (revision != _latest_revision) {
    //    LOGGER(_logger, V4_VVER, "discard obsolete result %s for rev. %i\n", resultString, revision);
    //    return;
    //}

    LOGGER(_logger, V3_VERB, "found result %s for rev. %i\n", resultString, revision);
    task.result.result = SatResult(res);
    task.result.revision = revision;
    if (res == SAT) { 
        auto solution = _solver.getSolution();
        task.result.setSolutionToSerialize(solution.data(), solution.size());
    } else {
        auto failed = _solver.getFailedAssumptions();
        auto failedVec = std::vector<int>(failed.begin(), failed.end());
        task.result.setSolutionToSerialize(failedVec.data(), failedVec.size());
    }

    // If necessary, convert solution back to original variable domain
    if (!_vt.getExtraVariables().empty()) {
        std::vector<int> origSolution;
        for (size_t i = 0; i < task.result.getSolutionSize(); i++) {
            if (res == UNSAT) {
                // Failed assumption
                origSolution.push_back(_vt.getOrigLitOrZero(task.result.getSolution(i)));
            } else if (i > 0) {
                // Assignment: v or -v at position v
                assert(task.result.getSolution(i) == i || task.result.getSolution(i) == -i);
                int origLit = _vt.getOrigLitOrZero(task.result.getSolution(i));
                if (origLit != 0) origSolution.push_back(origLit);
                assert(origSolution[origSolution.size()-1] == origSolution.size()-1 
                    || origSolution[origSolution.size()-1] == 1-origSolution.size());
            } else origSolution.push_back(0); // position zero
        }
        task.result.setSolutionToSerialize(origSolution.data(), origSolution.size());
    }

    task.hasResultToPublish = true;
}

SolverThread::~SolverThread() {
    if (_thread.joinable()) _thread.join();
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.getGlobalId());
    return _name.c_str();
}