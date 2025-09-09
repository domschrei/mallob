
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
#include "app/sat/solvers/solving_replay.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/string_utils.hpp"
#include "util/sys/proc.hpp"
#include "util/hashing.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "app/sat/data/definitions.hpp"
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

    std::string outPath = "witness-trace." + std::to_string(_solver.getSolverSetup().jobId) + "." + std::to_string(_solver.getGlobalId()) + ".txt";
    if (_lrat) {
        _lrat->getChecker().init();
        _lrat->setWitnessOutputPath(outPath);
    }
    if (_solver.getSolverSetup().owningModelCheckingLratConnector) {
        _solver.getSolverSetup().modelCheckingLratConnector->getChecker().init();
        _solver.getSolverSetup().modelCheckingLratConnector->setWitnessOutputPath(outPath);
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

    SerializedFormulaParser* fParser;

    while (true) {
        // Fetch the next formula to read
        {
            auto lock = _state_mutex.getLock();
            assert(_active_revision < (int)_pending_formulae.size());
            fParser = _pending_formulae[_active_revision].get();
        }

        // Forward raw formula data to LRAT connectors
        if (_lrat) {
            _lrat->initiateRevision(_active_revision, *fParser);
        }
        if (_solver.getSolverSetup().owningModelCheckingLratConnector) {
            _solver.getSolverSetup().modelCheckingLratConnector->initiateRevision(_active_revision, *fParser);
        }

        LOGGER(_logger, V4_VVER, "Reading rev. %i, start %i\n", (int)_active_revision, (int)_imported_lits_curr_revision);
        
        // Read literals, checking in between whether to stop/terminate
        int lit;
        while (fParser->getNextLiteral(lit)) {

            if (std::abs(lit) > (1<<30)) {
                LOGGER(_logger, V0_CRIT, "[ERROR] Invalid literal %i at rev. %i pos. %ld/%ld.\n",
                    lit, (int)_active_revision, _imported_lits_curr_revision, fParser->getPayloadSize());
                _logger.flush();
                abort();
            }
            if (lit == 0 && _last_read_lit_zero) {
                LOGGER(_logger, V0_CRIT, "[ERROR] Empty clause at rev. %i pos. %ld/%ld.\n", 
                    (int)_active_revision, _imported_lits_curr_revision, fParser->getPayloadSize());
                _logger.flush();
                abort();
            }
            _solver.addLiteral(lit);
            if (_params.useChecksums()) _running_chksum.combine(lit);
            _max_var = std::max(_max_var, std::abs(lit));
            _last_read_lit_zero = lit == 0;
            //_dbg_lits += std::to_string(lit]) + " ";
            //if (lit == 0) _dbg_lits += "\n";

            ++_imported_lits_curr_revision;
            // Suspend and/or terminate if needed
            if (_terminated) return false;
        }

        fParser->verifyChecksum();

        {
            auto lock = _state_mutex.getLock();

            _solver.setCurrentRevision(_active_revision);
            
            // No formula left to read?
            if (_active_revision == _latest_revision) {
            
                // Parse assumptions
                _pending_assumptions.clear();
                while (fParser->getNextAssumption(lit)) {
                    _pending_assumptions.push_back(lit);
                    // Adjust _max_var according to assumptions as well
                    _max_var = std::max(_max_var, std::abs(lit));
                }
            
                LOGGER(_logger, V4_VVER, "Reading done @ rev. %i (%lu lits, %lu assumptions)\n", (int)_active_revision,
                    _imported_lits_curr_revision, _pending_assumptions.size());
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
            new SerializedFormulaParser(_logger, data.fLits, data.fSize, _solver.getSolverSetup().onTheFlyChecking)
        );
        if (_params.compressFormula()) {
            _pending_formulae.back()->setCompressed();
            LOGGER(_logger, V4_VVER, "Received compressed formula of size %i\n", data.fSize);
        } else {
            LOGGER(_logger, V4_VVER, "Received %i literals: %s\n", data.fSize, StringUtils::getSummary(data.fLits, data.fSize).c_str());
        }
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
        aLits = _pending_assumptions.data();
        aSize = _pending_assumptions.size();
        _in_solve_call = true;
        if (revision < _latest_revision) {
            _solver.interrupt(); // revision obsolete: stop solving immediately
            performSolving = false; // actually just "pretend" to solve ...
        } else {
            _solver.uninterrupt(); // make sure solver isn't in an interrupted state
            chksum = _latest_checksum; // this checksum belongs to _latest_revision.
        }
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

    if (performSolving) {
        if (_solver.getReplay().getMode() == SolvingReplay::RECORD)
            _solver.getReplay().recordReturnFromSolve(res);
        if (_solver.getReplay().getMode() == SolvingReplay::REPLAY)
            res = SatResult(_solver.getReplay().replayReturnFromSolve(res));
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
            lrat->push(LratOp(solution.data()+1, solution.size()-1), true, revision);
            // Whether or not this was successful (another thread might have been earlier),
            // wait until SAT was validated.
            lrat->waitForConclusion(revision);
        }
        _state_mutex.lock();
        _result.setSolutionToSerialize(solution.data(), solution.size());
    } else {
        auto failed = _solver.getFailedAssumptions();
        auto failedVec = std::vector<int>(failed.begin(), failed.end());
        if (_lrat) {
            LOGGER(_logger, V3_VERB, "Validating UNSAT ...\n");
            _lrat->push(LratOp(_solver.getUnsatConclusionId(), failedVec.data(), failedVec.size()), true, revision);
            _lrat->waitForConclusion(revision);
        }
        _state_mutex.lock();
        _result.setSolutionToSerialize(failedVec.data(), failedVec.size());
    }
    _result.result = SatResult(res);
    _result.revision = revision;
    LOGGER(_logger, V3_VERB, "found result %s for rev. %i\n", resultString, revision);

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