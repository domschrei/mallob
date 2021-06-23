
#include <sys/resource.h>
#include <assert.h>

#include "app/sat/hordesat/solvers/solver_thread.hpp"
#include "app/sat/hordesat/horde.hpp"
#include "app/sat/hordesat/utilities/hash.hpp"
#include "util/sys/proc.hpp"

using namespace SolvingStates;

SolverThread::SolverThread(const Parameters& params, const HordeConfig& config,
         std::shared_ptr<PortfolioSolverInterface> solver, 
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        int localId) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(_solver.getLogger()), _shuffler(fSize, fLits), _local_id(localId), 
    _has_pseudoincremental_solvers(solver->getSolverSetup().hasPseudoincrementalSolvers) {
    
    _portfolio_rank = config.apprank;
    _portfolio_size = config.mpisize;
    _local_solvers_count = config.threads;

    appendRevision(0, fSize, fLits, aSize, aLits);
    _result.result = UNKNOWN;
}

void SolverThread::start() {
    if (_tid == -1) _thread = std::thread([this]() {
        init();
        run();
    });
}

void SolverThread::init() {
    _tid = Proc::getTid();
    _logger.log(V5_DEBG, "tid %ld\n", _tid);
    _initialized = true;
    
    _active_revision = 0;
    _imported_lits_curr_revision = 0;
}

void* SolverThread::run() {

    diversifyInitially();        

    while (!_terminated) {

        // Sleep and wait if the solver should not do solving right now
        waitWhileSolved();
        waitWhileSuspended();
        waitWhileInterrupted();
        
        bool readingDone = readFormula();

        // Skip solving attempt if interrupt is set
        if (!readingDone || _interrupted) continue;
        
        diversifyAfterReading();
        _shuffler = ClauseShuffler(0, nullptr);

        runOnce();
    }

    _logger.log(V4_VVER, "exiting\n");
    return NULL;
}

bool SolverThread::readFormula() {
    constexpr int batchSize = 100000;

    size_t fSize = 0;
    const int* fLits;
    size_t aSize = 0;
    const int* aLits;

    /*
    // TODO Include

    if (_shuffle) {

        while (!cancelThread() && _shuffler.hasNextClause()) {
            for (int lit : _shuffler.nextClause()) {
                _solver.addLiteral(lit);
                _imported_lits++;
            }
        }
    }
    */

    while (true) {

        // Fetch the next formula to read
        {
            auto lock = _state_mutex.getLock();
            assert(_active_revision < (int)_pending_formulae.size());
            fSize = _pending_formulae[_active_revision].first;
            fLits = _pending_formulae[_active_revision].second;
            aSize = _pending_assumptions[_active_revision].first;
            aLits = _pending_assumptions[_active_revision].second;
        }

        _logger.log(V4_VVER, "Reading rev. %i, start %i\n", (int)_active_revision, (int)_imported_lits_curr_revision);

        // Read the formula in batches from the point where you left off
        for (size_t start = _imported_lits_curr_revision; start < fSize; start += batchSize) {
            
            waitWhileSuspended();
            if (_interrupted) {
                _logger.log(V4_VVER, "Reading interrupted @ %i\n", (int)_imported_lits_curr_revision);
                return false;
            }

            size_t end = std::min(start+batchSize, fSize);
            for (size_t i = start; i < end; i++) {
                if (std::abs(fLits[i]) > 134217723) {
                    _logger.log(V0_CRIT, "ERROR of domain at rev. %i pos. %ld/%ld. Last %i literals: %i %i %i %i %i\n", 
                        (int)_active_revision, i, fSize,
                        (int) std::min(i+1, (size_t)5),
                        i >= 4 ? fLits[i-4] : 0,
                        i >= 3 ? fLits[i-3] : 0,
                        i >= 2 ? fLits[i-2] : 0,
                        i >= 1 ? fLits[i-1] : 0,
                        fLits[i]
                    );
                    abort();
                }
                _solver.addLiteral(_vt.getTldLit(fLits[i]));
                _max_var = std::max(_max_var, std::abs(fLits[i]));
                _imported_lits_curr_revision++;
                //_dbg_lits += std::to_string(fLits[i]) + " ";
                //if (fLits[i] == 0) _dbg_lits += "\n";
            }
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
                _logger.log(V4_VVER, "Encoding equivalence for %i assumptions of rev. %i/%i @ var. %i\n", 
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

            // No formula left to read?
            if (_active_revision == _latest_revision) {
                _logger.log(V4_VVER, "Reading done @ rev. %i\n", (int)_active_revision);                
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
        _logger.log(V4_VVER, "Received %i literals\n", fSize);
        _pending_assumptions.emplace_back(aSize, aLits);
        _logger.log(V4_VVER, "Received %i assumptions\n", aSize);
        _latest_revision = revision;
        _found_result = false;
        assert(_latest_revision+1 == (int)_pending_formulae.size() 
            || log_return_false("%i != %i", _latest_revision+1, _pending_formulae.size()));
        _solver.interrupt();
    }
    _state_cond.notify();
}

void SolverThread::diversifyInitially() {

    // Random seed: will be the same whenever rank and size stay the same,
    // changes to something completely new when rank or size change. 
    unsigned int seed = 42;
    hash_combine<unsigned int>(seed, (int)_tid);
    hash_combine<unsigned int>(seed, _portfolio_size);
    hash_combine<unsigned int>(seed, _portfolio_rank);
    srand(seed);
    _solver.diversify(seed);

    /*
    // TODO Include
    
    // Shuffle input
    // ... only if original diversifications are exhausted
    _shuffle = _solver.getDiversificationIndex() >= _solver.getNumOriginalDiversifications();
    float random = 0.001f * (rand() % 1000); // random number in [0,1)
    assert(random >= 0); assert(random <= 1);
    // ... only if random throw hits user-defined probability
    _shuffle = _shuffle && random < _params.getFloatParam("shufinp");
    if (_shuffle) {
        _logger.log(V4_VVER, "Shuffling input\n");
        _shuffler.doShuffle();
    }
    */
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
        if (_interrupted || _suspended) return;

        // Make solver ready to solve by removing interrupt and suspend flags
        _solver.uninterrupt();
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
    _logger.log(V4_VVER, "BEGSOL rev. %i (%i assumptions)\n", revision, aSize);
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
    _logger.log(V4_VVER, "ENDSOL\n");

    // Report result, if present
    reportResult(res);
}

void SolverThread::waitWhileSolved() {
    waitUntil([&]{return !_found_result;});
}
void SolverThread::waitWhileInterrupted() {
    waitUntil([&]{return !_interrupted;});
}
void SolverThread::waitWhileSuspended() {
    waitUntil([&]{return !_suspended;});
}

void SolverThread::waitUntil(std::function<bool()> predicate) {
    if (predicate()) return;
    _state_cond.wait(_state_mutex, predicate);
}

void SolverThread::reportResult(int res) {

    if (res == 0 || _found_result || _interrupted) return;

    auto lock = _state_mutex.getLock();

    int revision = _active_revision;
    const char* resultString = res==SAT?"SAT":"UNSAT";

    if (_active_revision != _latest_revision) {
        _logger.log(V4_VVER, "discard obsolete result %s for rev. %i\n", resultString, revision);
        return;
    }

    _logger.log(V4_VVER, "found result %s for rev. %i\n", resultString, revision);
    _result.result = SatResult(res);
    _result.revision = revision;
    if (res == SAT) { 
        _result.solution = _solver.getSolution();
    } else {
        auto failed = _solver.getFailedAssumptions();
        _result.solution = std::vector<int>(failed.begin(), failed.end());
    }

    // If necessary, convert solution back to original variable domain
    if (!_vt.getExtraVariables().empty()) {
        std::vector<int> origSolution;
        for (size_t i = 0; i < _result.solution.size(); i++) {
            if (res == UNSAT) {
                // Failed assumption
                origSolution.push_back(_vt.getOrigLitOrZero(_result.solution[i]));
            } else if (i > 0) {
                // Assignment: v or -v at position v
                assert(_result.solution[i] == i || _result.solution[i] == -i);
                int origLit = _vt.getOrigLitOrZero(_result.solution[i]);
                if (origLit != 0) origSolution.push_back(origLit);
                assert(origSolution[origSolution.size()-1] == origSolution.size()-1 
                    || origSolution[origSolution.size()-1] == 1-origSolution.size());
            } else origSolution.push_back(0); // position zero
        }
        _result.solution = std::move(origSolution);
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