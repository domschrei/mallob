
#include <assert.h>
#include <bits/std_abs.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <algorithm>
#include <set>

#include "app/sat/data/revision_data.hpp"
#include "app/sat/job/sat_constants.h"
#include "solver_thread.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/string_utils.hpp"
#include "util/sys/proc.hpp"
#include "util/hashing.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/execution/variable_translator.hpp"
#include "app/sat/job/sat_process_config.hpp"
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/option.hpp"
#include "util/params.hpp"

SolverThread::SolverThread(const Parameters& params, const SatProcessConfig& config,
         std::shared_ptr<PortfolioSolverInterface> solver, RevisionData firstRevision, int localId) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(_solver.getLogger()),
    _lrat(_solver.getSolverSetup().onTheFlyChecking ? _solver.getLratConnector() : nullptr),
    _local_id(localId),
    _has_pseudoincremental_solvers(solver->getSolverSetup().hasPseudoincrementalSolvers) {
    
    _portfolio_rank = config.apprank;
    _portfolio_size = config.mpisize;
    _local_solvers_count = config.threads;

    appendRevision(0, firstRevision);
    _result.result = UNKNOWN;

    if (_lrat && _params.derivationErrorChancePerMille() > 0) {
        _lrat->setTamperingChancePerMille(_params.derivationErrorChancePerMille());
    }
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

    if (_lrat || _solver.getSolverSetup().owningModelCheckingLratConnector) {
        // Convert hex string back to byte array
        signature target;
        {
            std::string sigStr = _solver.getSolverSetup().sigFormula;
            const char* src = sigStr.c_str();
            uint8_t* dest = target;
            while(*src && src[1]) {
                char c1 = src[0];
                int i1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 - 'a' + 10);
                char c2 = src[1];
                int i2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 - 'a' + 10);
                *(dest++) = i1*16 + i2;
                src += 2;
            }
        }
        if (_lrat) _lrat->getChecker().init(target);
        if (_solver.getSolverSetup().owningModelCheckingLratConnector)
            _solver.getSolverSetup().modelCheckingLratConnector->getChecker().init(target);
    }

    _initialized = true;
}

void* SolverThread::run() {

    diversifyInitially();        

    while (!_terminated) {

        // Sleep and wait if the solver should not do solving right now
        waitWhileSolved();
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

    SerializedFormulaParser* fParser;
    size_t aSize = 0;
    const int* aLits;

    while (true) {

        // Shuffle next input
        if (_imported_lits_curr_revision == 0) {
            // ... not for the first solver
            bool shuffle = _solver.getGlobalId() >= 10;
            float random = 0.0001f * (rand() % 10000); // random number in [0,1)
            assert(random >= 0); assert(random <= 1);
            // ... only if random throw hits user-defined probability
            shuffle = shuffle && _params.inputShuffleProbability() > 0
                && random <= _params.inputShuffleProbability();

            if (shuffle) {
                LOGGER(_logger, V4_VVER, "Shuffling input rev. %i\n", (int)_active_revision);
                {
                    auto lock = _state_mutex.getLock();
                    assert(_active_revision < (int)_pending_formulae.size());
                    fParser = _pending_formulae[_active_revision].get();
                }
                fParser->shuffle(_solver.getGlobalId());
            }
        }

        // Fetch the next formula to read
        {
            auto lock = _state_mutex.getLock();
            assert(_active_revision < (int)_pending_formulae.size());
            fParser = _pending_formulae[_active_revision].get();
            aSize = _pending_assumptions[_active_revision].first;
            aLits = _pending_assumptions[_active_revision].second;
        }

        // Forward raw formula data to LRAT connector
        if (_lrat) _lrat->launch(fParser->getRawPayload(), fParser->getPayloadSize());
        if (_solver.getSolverSetup().owningModelCheckingLratConnector)
            _solver.getSolverSetup().modelCheckingLratConnector->launch(fParser->getRawPayload(), fParser->getPayloadSize());

        LOGGER(_logger, V4_VVER, "Reading rev. %i, start %i\n", (int)_active_revision, (int)_imported_lits_curr_revision);
        
        // Repeatedly read a batch of literals, checking in between whether to stop/terminate
        while (_imported_lits_curr_revision < fParser->getPayloadSize()) {

            // Read next batch
            auto numImportedBefore = _imported_lits_curr_revision;
            auto end = std::min(numImportedBefore + batchSize, fParser->getPayloadSize());
            int lit;
            while (_imported_lits_curr_revision < end && fParser->getNextLiteral(lit)) {

                if (std::abs(lit) > (1<<30)) {
                    LOGGER(_logger, V0_CRIT, "[ERROR] Invalid literal %i at rev. %i pos. %ld/%ld.\n",
                        lit, (int)_active_revision, _imported_lits_curr_revision, fParser->getPayloadSize());
                    abort();
                }
                if (lit == 0 && _last_read_lit_zero) {
                    LOGGER(_logger, V0_CRIT, "[ERROR] Empty clause at rev. %i pos. %ld/%ld.\n", 
                        (int)_active_revision, _imported_lits_curr_revision, fParser->getPayloadSize());
                    abort();
                }
                _solver.addLiteral(_vt.getTldLit(lit));
                if (_params.useChecksums()) _running_chksum.combine(_vt.getTldLit(lit));
                _max_var = std::max(_max_var, std::abs(lit));
                _last_read_lit_zero = lit == 0;
                //_dbg_lits += std::to_string(lit]) + " ";
                //if (lit == 0) _dbg_lits += "\n";

                ++_imported_lits_curr_revision;
            }

            // Suspend and/or terminate if needed
            if (_terminated) return false;
        }
        // Adjust _max_var according to assumptions as well
        for (size_t i = 0; i < aSize; i++) _max_var = std::max(_max_var, std::abs(aLits[i]));

        fParser->verifyChecksum();

        {
            auto lock = _state_mutex.getLock();
            assert(_imported_lits_curr_revision == fParser->getPayloadSize());

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
                    _solver.addConditionalLit(aSize > 0 ? -aEquivVar : 0);
                
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
                LOGGER(_logger, V4_VVER, "Reading done @ rev. %i (%lu lits)\n", (int)_active_revision,
                    _imported_lits_curr_revision);
                return true;
            }
            _active_revision++;
            _imported_lits_curr_revision = 0;
        }
    }
}

void SolverThread::appendRevision(int revision, RevisionData data) {
    {
        auto lock = _state_mutex.getLock();
        _pending_formulae.emplace_back(
            new SerializedFormulaParser(
                _logger, data.fSize, data.fLits,
                revision == 0 ? _solver.getSolverSetup().numOriginalClauses : 0
            )
        );
        LOGGER(_logger, V4_VVER, "Received %i literals: %s\n", data.fSize, StringUtils::getSummary(data.fLits, data.fSize).c_str());
        _pending_assumptions.emplace_back(data.aSize, data.aLits);
        LOGGER(_logger, V4_VVER, "Received %i assumptions: %s\n", data.aSize, StringUtils::getSummary(data.aLits, data.aSize).c_str());
        _latest_revision = revision;
        _latest_checksum = data.chksum;
        _found_result = false;
        assert(_latest_revision+1 == (int)_pending_formulae.size() 
            || LOG_RETURN_FALSE("%i != %i", _latest_revision+1, _pending_formulae.size()));
        if (_in_solve_call) {
            assert(revision > _active_revision);
            _solver.interrupt();
        }
    }
    _state_cond.notify();
}

void SolverThread::diversifyInitially() {

    // Random seed
    size_t seed = _params.seed();
    if (_params.diversifySeeds()) {
        seed += _solver.getGlobalId();
        //hash_combine(seed, (unsigned int)_tid);
        hash_combine(seed, (unsigned int)_portfolio_size);
        hash_combine(seed, (unsigned int)_portfolio_rank);
    }
    // Diversify solver based on seed
    _solver.diversify(seed);
    // RNG
    _rng = SplitMix64Rng(seed+_solver.getGlobalId());
}

void SolverThread::diversifyAfterReading() {
    if (!_params.diversifyPhases()) return;
    if (_solver.getGlobalId() < _solver.getNumOriginalDiversifications()) return;

    int solversCount = _local_solvers_count;
    int totalSolvers = solversCount * _portfolio_size;
    int vars = _solver.getVariablesCount();

    for (int var = 1; var <= vars; var++) {
        float numSolversRand = _rng.randomInRange(0, totalSolvers);
        if (numSolversRand < 1) {
            _solver.setPhase(var, numSolversRand < 0.5);
        }
    }
}

void SolverThread::runOnce() {

    // Set up correct solver state
    size_t aSize;
    const int* aLits;
    int revision;
    Checksum chksum {};
    bool performSolving = true;
    {
        auto lock = _state_mutex.getLock();

        // Set assumptions for upcoming solving attempt, set correct revision
        revision = _active_revision;
        auto asmpt = _pending_assumptions.at(revision);
        aSize = asmpt.first; aLits = asmpt.second;
        _in_solve_call = true;
        if (revision < _latest_revision) {
            _solver.interrupt(); // revision obsolete: stop solving immediately
            performSolving = false; // actually just "pretend" to solve ...
        } else {
            _solver.uninterrupt(); // make sure solver isn't in an interrupted state
            chksum = _latest_checksum; // this checksum belongs to _latest_revision.
        }
    }

    // If necessary, translate assumption literals
    std::vector<int> tldAssumptions;
    if (!_vt.getExtraVariables().empty()) {
        for (size_t i = 0; i < aSize; i++) {
            tldAssumptions.push_back(_vt.getTldLit(aLits[i]));
        }
        aLits = tldAssumptions.data();
    }

    // append assumption literals to formula hash
    auto hash = _running_chksum;
    if (_params.useChecksums()) for (int i = 0; i < aSize; i++) hash.combine(aLits[i]);

    // Ensure that the checksums match (except if we're not in the latest revision)
    if (chksum.count() > 0 && chksum != hash) {
        LOGGER(_logger, V0_CRIT, "[ERROR] Checksum fail: expected %lu,%x - computed %lu,%x\n",
            chksum.count(), chksum.get(), hash.count(), hash.get());
        abort();
    }

    // Perform solving (blocking)
    LOGGER(_logger, V4_VVER, "BEGSOL rev. %i (chk %lu,%x, %i assumptions): %s\n", revision, hash.count(), hash.get(),
        aSize, StringUtils::getSummary(aLits, aSize).c_str());
    //std::ofstream ofs("DBG_" + std::to_string(_solver.getGlobalId()) + "_" + std::to_string(_active_revision));
    //ofs << _dbg_lits << "\n";
    //ofs.close();
    SatResult res;
    if (_solver.supportsIncrementalSat()) {
        res = performSolving ? _solver.solve(aSize, aLits) : UNKNOWN;
    } else {
        // Add assumptions as permanent unit clauses
        for (const int* aLit = aLits; aLit != aLits+aSize; aLit++) {
            _solver.addLiteral(*aLit);
            _solver.addLiteral(0);
        }
        res = performSolving ? _solver.solve(0, nullptr) : UNKNOWN;
    }
    // Uninterrupt solver (if it was interrupted)
    {
        auto lock = _state_mutex.getLock();
        _in_solve_call = false;
        _solver.uninterrupt();
    }
    LOGGER(_logger, V4_VVER, "ENDSOL\n");

    // Report result, if present
    if (res == UNSAT && _solver.getOptimizer()) {
        res = UNKNOWN;
        _solver.getOptimizer()->set_lower_bound_proven(); // exports proven bound to other solvers
        if (_solver.getOptimizer()->has_best_solution()) {
            // this solver is the one to report the optimal result
            LOGGER(_logger, V2_INFO, "BEST FOUND SOLUTION COST: %lu\n",
                std::abs(_solver.getOptimizer()->best_objective_found_so_far()));
            res = SAT;
        }
    }
    reportResult(res, revision);
}

void SolverThread::waitWhileSolved() {
    waitUntil([&]{return _terminated || !_found_result;});
}

void SolverThread::waitUntil(std::function<bool()> predicate) {
    if (predicate()) return;
    _state_cond.wait(_state_mutex, predicate);
}

void SolverThread::reportResult(int res, int revision) {

    if (res == 0 || _found_result) return;
    const char* resultString = res==SAT?"SAT":"UNSAT";

    {
        auto lock = _state_mutex.getLock();

        if (revision != _latest_revision) {
            LOGGER(_logger, V4_VVER, "discard obsolete result %s for rev. %i\n", resultString, revision);
            return;
        }

        if (res == RESULT_UNSAT && _solver.getSolverSetup().avoidUnsatParticipation) {
            LOGGER(_logger, V4_VVER, "ignore uncertified UNSAT result\n");
            _terminated = true;
            return;
        }
    }

    if (res == SAT) {
        auto solution = _solver.getOptimizer() ?
            _solver.getOptimizer()->get_solution() : _solver.getSolution();
        auto lrat = _solver.getSolverSetup().modelCheckingLratConnector;
        if (lrat) {
            LOGGER(_logger, V3_VERB, "Validating SAT ...\n");
            // omit first "0" in solution vector
            lrat->setSolution(solution.data()+1, solution.size()-1);
            // Whether or not this was successful (another thread might have been earlier),
            // wait until SAT was validated.
            lrat->waitForSatValidation();
        }
        _state_mutex.lock();
        _result.setSolutionToSerialize(solution.data(), solution.size());
    } else {
        if (_lrat) {
            LOGGER(_logger, V3_VERB, "Validating UNSAT ...\n");
            _lrat->push({20});
            _lrat->waitForUnsatValidation();
        }
        auto failed = _solver.getFailedAssumptions();
        auto failedVec = std::vector<int>(failed.begin(), failed.end());
        _state_mutex.lock();
        _result.setSolutionToSerialize(failedVec.data(), failedVec.size());
    }
    _result.result = SatResult(res);
    _result.revision = revision;
    LOGGER(_logger, V3_VERB, "found result %s for rev. %i\n", resultString, revision);

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
    _solver.setFoundResult();
    _state_mutex.unlock();
}

SolverThread::~SolverThread() {
    if (_thread.joinable()) _thread.join();
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.getGlobalId());
    return _name.c_str();
}