
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
        int localId) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(_solver.getLogger()), _local_id(localId), 
    _has_pseudoincremental_solvers(solver->getSolverSetup().hasPseudoincrementalSolvers) {
    
    _portfolio_rank = config.apprank;
    _portfolio_size = config.mpisize;
    _local_solvers_count = config.threads;

    appendRevision(0, fSize, fLits, aSize, aLits);
    _result.result = UNKNOWN;
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
    
    _active_revision = 0;
    _imported_lits_curr_revision = 0;
    
    _initialized = true;
}

void* SolverThread::run() {

    diversifyInitially();        

    // Shuffle input
    // ... only if original diversifications are exhausted
    _shuffle = _solver.getDiversificationIndex() >= _solver.getNumOriginalDiversifications();
    float random = 0.001f * (rand() % 1000); // random number in [0,1)
    assert(random >= 0); assert(random <= 1);
    // ... only if random throw hits user-defined probability
    _shuffle = _shuffle && random < _params.inputShuffleProbability();

    while (!_terminated) {

        // Sleep and wait if the solver should not do solving right now
        waitWhileSolved();
        waitWhileSuspended();
        if (_terminated) break;
        
        bool readingDone = readFormula();

        // Skip solving attempt if reading was incomplete
        if (!readingDone) continue;
        
        diversifyAfterReading();

        runOnce();
    }

    LOGGER(_logger, V4_VVER, "exiting\n");
    return NULL;
}

bool SolverThread::readFormula() {
    constexpr int batchSize = 100000;

    size_t fSize = 0;
    const int* fLits;
    size_t aSize = 0;
    const int* aLits;

    while (true) {

        // Shuffle input if necessary
        if (_imported_lits_curr_revision == 0 && _shuffle) {
            LOGGER(_logger, V4_VVER, "Shuffling input rev. %i\n", (int)_active_revision);
            {
                auto lock = _state_mutex.getLock();
                assert(_active_revision < (int)_pending_formulae.size());
                fSize = _pending_formulae[_active_revision].first;
                fLits = _pending_formulae[_active_revision].second;
            }
            auto [sData, sSize] = _shuffler.doShuffle(fLits, fSize);
            _pending_formulae[_active_revision].first = sSize;
            _pending_formulae[_active_revision].second = sData;
        }

        // Fetch the next formula to read
        {
            auto lock = _state_mutex.getLock();
            assert(_active_revision < (int)_pending_formulae.size());
            fSize = _pending_formulae[_active_revision].first;
            fLits = _pending_formulae[_active_revision].second;
            aSize = _pending_assumptions[_active_revision].first;
            aLits = _pending_assumptions[_active_revision].second;
        }

        LOGGER(_logger, V4_VVER, "Reading rev. %i, start %i\n", (int)_active_revision, (int)_imported_lits_curr_revision);
        
        // Read the formula in batches from the point where you left off
        for (size_t start = _imported_lits_curr_revision; start < fSize; start += batchSize) {

            size_t end = std::min(start+batchSize, fSize);
            for (size_t i = start; i < end; i++) {
                int lit = fLits[i];
                if (std::abs(lit) > 134217723) {
                    LOGGER(_logger, V0_CRIT, "[ERROR] Invalid literal at rev. %i pos. %ld/%ld. Last %i literals: %i %i %i %i %i\n", 
                        (int)_active_revision, i, fSize,
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
                        (int)_active_revision, i, fSize,
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
                _imported_lits_curr_revision++;
                //_dbg_lits += std::to_string(lit]) + " ";
                //if (lit == 0) _dbg_lits += "\n";
            }
            
            waitWhileSuspended();
            if (_terminated) return false;
        }
        for (size_t i = 0; i < aSize; i++) _max_var = std::max(_max_var, std::abs(aLits[i]));

        {
            auto lock = _state_mutex.getLock();
            assert(_imported_lits_curr_revision == fSize);

            // If necessary, introduce extra variable to the problem
            // to encode equivalence to the set of assumptions
            if (_has_pseudoincremental_solvers && _active_revision >= _vt.getExtraVariables().size()) {
                _vt.addExtraVariable(_max_var);
                int aEquivVar = _vt.getExtraVariables().back();
                LOGGER(_logger, V4_VVER, "Encoding equivalence for %i assumptions of rev. %i/%i @ var. %i\n", 
                    aSize, (int)_active_revision, (int)_latest_revision, (int)aEquivVar);
                
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

            _solver.setCurrentRevision(_active_revision);
            
            // No formula left to read?
            if (_active_revision == _latest_revision) {
                LOGGER(_logger, V4_VVER, "Reading done @ rev. %i\n", (int)_active_revision);                
                return true;
            }
            _active_revision++;
            _imported_lits_curr_revision = 0;
        }
    }
}

void SolverThread::appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits) {
    {
        auto lock = _state_mutex.getLock();
        _pending_formulae.emplace_back(fSize, fLits);
        LOGGER(_logger, V4_VVER, "Received %i literals\n", fSize);
        _pending_assumptions.emplace_back(aSize, aLits);
        LOGGER(_logger, V4_VVER, "Received %i assumptions\n", aSize);
        _latest_revision = revision;
        _found_result = false;
        assert(_latest_revision+1 == (int)_pending_formulae.size() 
            || LOG_RETURN_FALSE("%i != %i", _latest_revision+1, _pending_formulae.size()));
        if (revision > 0) {
            _solver.interrupt();
            _interrupted = true;
        }
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

void SolverThread::runOnce() {

    // Set up correct solver state (or return if not possible)
    size_t aSize;
    const int* aLits;
    int revision;
    {
        auto lock = _state_mutex.getLock();

        // Last point where upcoming solving attempt may be cancelled prematurely
        if (_suspended) return;

        // Make solver ready to solve by removing suspend flag
        _solver.resume();

        // Set assumptions for upcoming solving attempt, set correct revision
        auto asmpt = _pending_assumptions.back();
        aSize = asmpt.first; aLits = asmpt.second;
        revision = _active_revision;
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

    // Report result, if present
    reportResult(res, revision);
}

void SolverThread::waitWhileSolved() {
    waitUntil([&]{return _terminated || !_found_result;});
}
void SolverThread::waitWhileSuspended() {
    waitUntil([&]{return _terminated || !_suspended;});
}

void SolverThread::waitUntil(std::function<bool()> predicate) {
    if (predicate()) return;
    _state_cond.wait(_state_mutex, predicate);
}

void SolverThread::reportResult(int res, int revision) {

    if (res == 0 || _found_result) return;
    const char* resultString = res==SAT?"SAT":"UNSAT";

    auto lock = _state_mutex.getLock();

    if (revision != _latest_revision) {
        LOGGER(_logger, V4_VVER, "discard obsolete result %s for rev. %i\n", resultString, revision);
        return;
    }

    LOGGER(_logger, V3_VERB, "found result %s for rev. %i\n", resultString, revision);
    _result.result = SatResult(res);
    _result.revision = revision;
    if (res == SAT) { 
        auto solution = _solver.getSolution();
        _result.setSolutionToSerialize(solution.data(), solution.size());
    } else {
        auto failed = _solver.getFailedAssumptions();
        auto failedVec = std::vector<int>(failed.begin(), failed.end());
        _result.setSolutionToSerialize(failedVec.data(), failedVec.size());
    }

    // If necessary, convert solution back to original variable domain
    if (!_vt.getExtraVariables().empty()) {
        std::vector<int> origSolution;
        for (size_t i = 0; i < _result.getSolutionSize(); i++) {
            if (res == UNSAT) {
                // Failed assumption
                origSolution.push_back(_vt.getOrigLitOrZero(_result.getSolution(i)));
            } else if (i > 0) {
                // Assignment: v or -v at position v
                assert(_result.getSolution(i) == i || _result.getSolution(i) == -i);
                int origLit = _vt.getOrigLitOrZero(_result.getSolution(i));
                if (origLit != 0) origSolution.push_back(origLit);
                assert(origSolution[origSolution.size()-1] == origSolution.size()-1 
                    || origSolution[origSolution.size()-1] == 1-origSolution.size());
            } else origSolution.push_back(0); // position zero
        }
        _result.setSolutionToSerialize(origSolution.data(), origSolution.size());
    }

    _found_result = true;
}

SolverThread::~SolverThread() {
    if (_thread.joinable()) _thread.join();
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.getGlobalId());
    return _name.c_str();
}