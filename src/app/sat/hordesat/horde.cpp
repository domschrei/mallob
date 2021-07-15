/*
 * This is a heavily modified successor of HordeSat's entry point.
 * Original class created on: Mar 24, 2017, Author: balyo
 */

#include "horde.hpp"

#include "utilities/debug_utils.hpp"
#include "sharing/default_sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "solvers/cadical.hpp"
#include "solvers/lingeling.hpp"
#include "solvers/mergesat.hpp"
#ifdef MALLOB_USE_RESTRICTED
#include "solvers/glucose.hpp"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <assert.h>

using namespace SolvingStates;

HordeLib::HordeLib(const Parameters& params, const HordeConfig& config, Logger&& loggingInterface) : 
			_params(params), _config(config), _logger(std::move(loggingInterface)), _state(INITIALIZING) {
	
    int appRank = config.apprank;

	_logger.log(V2_INFO, "Hlib engine on job %s\n", config.getJobStr().c_str());
	//params.printParams();
	_num_solvers = config.threads;
	_job_id = config.jobid;
	
	// Retrieve the string defining the cycle of solver choices, one character per solver
	// e.g. "llgc" => lingeling lingeling glucose cadical lingeling lingeling glucose ...
	std::string solverChoices = params.satSolverSequence();
	
	// These numbers become the diversifier indices of the solvers on this node
	int numLgl = 0;
	int numGlu = 0;
	int numCdc = 0;
	int numMrg = 0;

	// Add solvers from full cycles on previous ranks
	// and from the begun cycle on the previous rank
	int numFullCycles = (appRank * _num_solvers) / solverChoices.size();
	int begunCyclePos = (appRank * _num_solvers) % solverChoices.size();
	bool hasPseudoincrementalSolvers = false;
	for (size_t i = 0; i < solverChoices.size(); i++) {
		int* solverToAdd;
		bool pseudoIncremental = islower(solverChoices[i]);
		if (pseudoIncremental) hasPseudoincrementalSolvers = true;
		switch (solverChoices[i]) {
		case 'l': case 'L': solverToAdd = &numLgl; break;
		case 'g': case 'G': solverToAdd = &numGlu; break;
		case 'c': case 'C': solverToAdd = &numCdc; break;
		case 'm': case 'M': solverToAdd = &numMrg; break;
		}
		*solverToAdd += numFullCycles + (i < begunCyclePos);
	}

	// Solver-agnostic options each solver in the portfolio will receive
	SolverSetup setup;
	setup.logger = &_logger;
	setup.jobname = config.getJobStr();
	setup.isJobIncremental = config.incremental;
	setup.useAdditionalDiversification = params.addOldLglDiversifiers();
	setup.hardInitialMaxLbd = params.initialHardLbdLimit();
	setup.hardFinalMaxLbd = params.finalHardLbdLimit();
	setup.softInitialMaxLbd = params.initialSoftLbdLimit();
	setup.softFinalMaxLbd = params.finalSoftLbdLimit();
	setup.hardMaxClauseLength = params.hardMaxClauseLength();
	setup.softMaxClauseLength = params.softMaxClauseLength();
	setup.anticipatedLitsToImportPerCycle = config.maxBroadcastedLitsPerCycle;
	setup.hasPseudoincrementalSolvers = setup.isJobIncremental && hasPseudoincrementalSolvers;

	// Instantiate solvers according to the global solver IDs and diversification indices
	int cyclePos = begunCyclePos;
	for (setup.localId = 0; setup.localId < _num_solvers; setup.localId++) {
		setup.globalId = appRank * _num_solvers + setup.localId;
		// Which solver?
		setup.solverType = solverChoices[cyclePos];
		setup.doIncrementalSolving = setup.isJobIncremental && !islower(setup.solverType);
		switch (setup.solverType) {
		case 'l': case 'L': setup.diversificationIndex = numLgl++; break;
		case 'c': case 'C': setup.diversificationIndex = numCdc++; break;
		case 'm': case 'M': setup.diversificationIndex = numMrg++; break;
		case 'g': case 'G': setup.diversificationIndex = numGlu++; break;
		}
		_solver_interfaces.emplace_back(createSolver(setup));
		cyclePos = (cyclePos+1) % solverChoices.size();
	}

	_sharing_manager.reset(new DefaultSharingManager(_solver_interfaces, _params, _logger));
	_logger.log(V5_DEBG, "initialized\n");
}

std::shared_ptr<PortfolioSolverInterface> HordeLib::createSolver(const SolverSetup& setup) {
	std::shared_ptr<PortfolioSolverInterface> solver;
	switch (setup.solverType) {
	case 'l':
	case 'L':
		// Lingeling
		_logger.log(V4_VVER, "S%i : Lingeling-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Lingeling(setup));
		break;
	case 'c':
	case 'C':
		// Cadical
		_logger.log(V4_VVER, "S%i : Cadical-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Cadical(setup));
		break;
	case 'm':
	//case 'M': // no support for incremental mode as of now
		// MergeSat
		_logger.log(V4_VVER, "S%i : MergeSat-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new MergeSatBackend(setup));
		break;
#ifdef MALLOB_USE_RESTRICTED
	case 'g':
	//case 'G': // no support for incremental mode as of now
		// Glucose
		_logger.log(V4_VVER, "S%i: Glucose-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new MGlucose(setup));
		break;
#endif
	default:
		// Invalid solver
		_logger.log(V2_INFO, "Fatal error: Invalid solver \"%c\" assigned\n", setup.solverType);
		_logger.flush();
		abort();
		break;
	}
	return solver;
}

void HordeLib::appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits) {
	assert(_state != ACTIVE);
	_logger.log(V4_VVER, "append rev. %i: %i lits, %i assumptions\n", revision, fSize, aSize);
	assert(_revision+1 == revision);
	_revision_data.push_back(RevisionData{fSize, fLits, aSize, aLits});
	
	for (size_t i = 0; i < _num_solvers; i++) {
		if (revision == 0) {
			// Initialize solver thread
			_solver_threads.emplace_back(new SolverThread(
				_params, _config, _solver_interfaces[i], fSize, fLits, aSize, aLits, i
			));
		} else {
			if (_solver_interfaces[i]->getSolverSetup().doIncrementalSolving) {
				_solver_threads[i]->appendRevision(revision, fSize, fLits, aSize, aLits);
			} else {
				// Pseudo-incremental SAT solving: 
				// Phase out old solver thread
				_sharing_manager->stopClauseImport(i);
				_solver_threads[i]->setTerminate();
				_obsolete_solver_threads.push_back(std::move(_solver_threads[i]));
				// Setup new solver  and new solver thread
				_solver_interfaces[i] = createSolver(_solver_interfaces[i]->getSolverSetup());
				_solver_threads[i] = std::shared_ptr<SolverThread>(new SolverThread(
					_params, _config, _solver_interfaces[i], 
					_revision_data[0].fSize, _revision_data[0].fLits, 
					_revision_data[0].aSize, _revision_data[0].aLits, 
					i
				));
				// Load entire formula 
				for (int importedRevision = 1; importedRevision <= revision; importedRevision++) {
					auto data = _revision_data[importedRevision];
					_solver_threads[i]->appendRevision(importedRevision, 
						data.fSize, data.fLits, data.aSize, data.aLits
					);
				}
				_sharing_manager->continueClauseImport(i);
				_solvers_started = false; // need to restart at least one solver
			}
		}
	}
	_revision = revision;
}

void HordeLib::solve() {
	assert(_revision >= 0);
	_result.result = UNKNOWN;
	setSolvingState(ACTIVE);
	if (!_solvers_started) {
		// Need to start threads
		_logger.log(V4_VVER, "starting threads\n");
		for (auto& thread : _solver_threads) thread->start();
		_solvers_started = true;
	}
}

bool HordeLib::isFullyInitialized() {
	if (_state == INITIALIZING) return false;
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		if (!_solver_threads[i]->isInitialized()) return false;
	}
	return true;
}

int HordeLib::solveLoop() {
	if (isCleanedUp()) return -1;

    // Solving done?
	bool done = false;
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		if (_solver_threads[i]->hasFoundResult(_revision)) {
			auto& result = _solver_threads[i]->getSatResult();
			if (result.result > 0 && result.revision == _revision) {
				done = true;
				_result = std::move(result);
				break;
			}
		}
	}

	if (done) {
		_logger.log(V5_DEBG, "Returning result\n");
		return _result.result;
	}
    return -1; // no result yet
}

int HordeLib::prepareSharing(int* begin, int maxSize, Checksum& checksum) {
	if (isCleanedUp()) return 0;
	_logger.log(V5_DEBG, "collecting clauses on this node\n");
	int size = _sharing_manager->prepareSharing(begin, maxSize);

	if (_params.useChecksums()) {
		checksum.combine(_job_id);
		for (int i = 0; i < size; i++) checksum.combine(begin[i]);
	}

	return size;
}

void HordeLib::digestSharing(std::vector<int>& result, const Checksum& checksum) {
	if (isCleanedUp()) return;

	if (_params.useChecksums()) {
		Checksum chk;
		chk.combine(_job_id);
		for (int lit : result) chk.combine(lit);
		if (chk.get() != checksum.get()) {
			_logger.log(V1_WARN, "[WARN] Checksum fail (expected count: %ld, actual count: %ld)\n", checksum.count(), chk.count());
			return;
		}
	}

	_sharing_manager->digestSharing(result);
}

void HordeLib::digestSharing(int* begin, int size, const Checksum& checksum) {
	if (isCleanedUp()) return;

	if (_params.useChecksums()) {
		Checksum chk;
		chk.combine(_job_id);
		for (int i = 0; i < size; i++) chk.combine(begin[i]);
		if (chk.get() != checksum.get()) {
			_logger.log(V1_WARN, "[WARN] Checksum fail (expected count: %ld, actual count: %ld)\n", checksum.count(), chk.count());
			return;
		}
	}

	_sharing_manager->digestSharing(begin, size);
}

void HordeLib::dumpStats(bool final) {
	if (isCleanedUp() || !isFullyInitialized()) return;

	// Local statistics
	SolvingStatistics locSolveStats;
	for (size_t i = 0; i < _num_solvers; i++) {
		SolvingStatistics st = _solver_interfaces[i]->getStatistics();
		_logger.log(V2_INFO, "%sS%d pps:%lu decs:%lu cnfs:%lu mem:%0.2f recv:%lu digd:%lu disc:%lu\n",
				final ? "END " : "",
				_solver_interfaces[i]->getGlobalId(), 
				st.propagations, st.decisions, st.conflicts, st.memPeak, 
				st.receivedClauses, st.digestedClauses, st.discardedClauses);
		locSolveStats.conflicts += st.conflicts;
		locSolveStats.decisions += st.decisions;
		locSolveStats.memPeak += st.memPeak;
		locSolveStats.propagations += st.propagations;
		locSolveStats.restarts += st.restarts;
	}
	SharingStatistics locShareStats;
	if (_sharing_manager != NULL) locShareStats = _sharing_manager->getStatistics();

	unsigned long exportedWithFailed = locShareStats.exportedClauses + locShareStats.clausesFilteredAtExport + locShareStats.clausesDroppedAtExport;
	unsigned long importedWithFailed = locShareStats.importedClauses + locShareStats.clausesFilteredAtImport;
	_logger.log(V2_INFO, "%spps:%lu decs:%lu cnfs:%lu mem:%0.2f exp:%lu/%lu(drp:%lu) imp:%lu/%lu\n",
			final ? "END " : "",
			locSolveStats.propagations, locSolveStats.decisions, locSolveStats.conflicts, locSolveStats.memPeak, 
			locShareStats.exportedClauses, exportedWithFailed, locShareStats.clausesDroppedAtExport, 
			locShareStats.importedClauses, importedWithFailed);

	if (final) {
		// Histogram over clause lengths (do not print trailing zeroes)
		std::string hist = "";
		std::string histZeroesOnly = "";
		for (size_t i = 1; i < CLAUSE_LEN_HIST_LENGTH; i++) {
			auto val = locShareStats.seenClauseLenHistogram[i];
			if (val > 0) {
				if (!histZeroesOnly.empty()) {
					// Flush sequence of zeroes into main string
					hist += histZeroesOnly;
					histZeroesOnly = "";
				}
				hist += " " + std::to_string(val);
			} else {
				// Append zero to side string
				histZeroesOnly += " " + std::to_string(val);
			}
		}
		if (!hist.empty()) _logger.log(V2_INFO, "END clenhist:%s\n", hist.c_str());

		// Flush logs
		for (auto& solver : _solver_interfaces) solver->getLogger().flush();
		_logger.flush();
	}
}

void HordeLib::setPaused() {
	if (_state == ACTIVE)	setSolvingState(SUSPENDED);
}

void HordeLib::unsetPaused() {
	if (_state == SUSPENDED) setSolvingState(ACTIVE);
}

void HordeLib::interrupt() {
	if (_state != STANDBY) {
		dumpStats(/*final=*/false);
		setSolvingState(STANDBY);
	}
}

void HordeLib::abort() {
	if (_state != ABORTING) {
		dumpStats(/*final=*/true);
		setSolvingState(ABORTING);
	}
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = _state;
	_state = state;
	_logger.log(V4_VVER, "state change %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);
	for (auto& solver : _solver_threads) {
		if (state == SolvingState::ABORTING) {
			solver->setTerminate();
		}
		solver->setSuspend(state == SolvingState::SUSPENDED);
		solver->setInterrupt(state == SolvingState::STANDBY);
	}
}

void HordeLib::cleanUp() {
	double time = Timer::elapsedSeconds();

	_logger.log(V5_DEBG, "[hlib-cleanup] enter\n");

	// for any running threads left:
	setSolvingState(ABORTING);
	
	// join and delete threads
	for (auto& thread : _solver_threads) thread->tryJoin();
	for (auto& thread : _obsolete_solver_threads) thread->tryJoin();
	_solver_threads.clear();
	_obsolete_solver_threads.clear();

	_logger.log(V5_DEBG, "[hlib-cleanup] joined threads\n");

	// delete solvers
	_solver_interfaces.clear();
	_logger.log(V5_DEBG, "[hlib-cleanup] cleared solvers\n");

	time = Timer::elapsedSeconds() - time;
	_logger.log(V4_VVER, "[hlib-cleanup] done, took %.3f s\n", time);
	_logger.flush();

	_cleaned_up = true;
}

HordeLib::~HordeLib() {
	if (!_cleaned_up) cleanUp();
}
