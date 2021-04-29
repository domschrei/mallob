/*
 * HordeLib.cpp
 *
 *  Created on: Mar 24, 2017
 *      Author: balyo
 */

#include "horde.hpp"

#include "utilities/debug_utils.hpp"
#include "sharing/default_sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "solvers/cadical.hpp"
#include "solvers/lingeling.hpp"
//#include "solvers/mergesat.hpp"
#ifdef MALLOB_USE_RESTRICTED
#include "solvers/glucose.hpp"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <assert.h>

using namespace SolvingStates;

HordeLib::HordeLib(const Parameters& params, Logger&& loggingInterface) : 
			_params(params), _logger(std::move(loggingInterface)), _state(INITIALIZING) {
	
    int appRank = params.getIntParam("apprank");

	_logger.log(V2_INFO, "Hlib engine on job %s\n", params.getParam("jobstr").c_str());
	//params.printParams();
	_num_solvers = params.getIntParam("threads", 1);
	_sleep_microsecs = 1000 * params.getIntParam("i", 1000);

	// Retrieve the string defining the cycle of solver choices, one character per solver
	// e.g. "llgc" => lingeling lingeling glucose cadical lingeling lingeling glucose ...
	std::string solverChoices = params.getParam("satsolver", "l");
	
	// These numbers become the diversifier indices of the solvers on this node
	int numLgl = 0;
	int numGlu = 0;
	int numCdc = 0;
	int numMrg = 0;

	// Add solvers from full cycles on previous ranks
	// and from the begun cycle on the previous rank
	int numFullCycles = (appRank * _num_solvers) / solverChoices.size();
	int begunCyclePos = (appRank * _num_solvers) % solverChoices.size();
	for (size_t i = 0; i < solverChoices.size(); i++) {
		int* solverToAdd;
		switch (solverChoices[i]) {
		case 'l': solverToAdd = &numLgl; break;
		case 'g': solverToAdd = &numGlu; break;
		case 'c': solverToAdd = &numCdc; break;
		case 'm': solverToAdd = &numMrg; break;
		}
		*solverToAdd += numFullCycles + (i < begunCyclePos);
	}

	// Solver-agnostic options each solver in the portfolio will receive
	SolverSetup setup;
	setup.logger = &_logger;
	setup.jobname = params.getParam("jobstr");
	setup.useAdditionalDiversification = params.isNotNull("aod");
	setup.hardInitialMaxLbd = params.getIntParam("ihlbd");
	setup.hardFinalMaxLbd = params.getIntParam("fhlbd");
	setup.softInitialMaxLbd = params.getIntParam("islbd");
	setup.softFinalMaxLbd = params.getIntParam("fslbd");
	setup.hardMaxClauseLength = params.getIntParam("hmcl");
	setup.softMaxClauseLength = params.getIntParam("smcl");
	setup.anticipatedLitsToImportPerCycle = params.getIntParam("mblpc");

	// Instantiate solvers according to the global solver IDs and diversification indices
	int cyclePos = begunCyclePos;
	for (setup.localId = 0; setup.localId < _num_solvers; setup.localId++) {
		setup.globalId = appRank * _num_solvers + setup.localId;
		// Which solver?
		switch (solverChoices[cyclePos]) {
		case 'l':
			// Lingeling
			setup.diversificationIndex = numLgl++;
			_logger.log(V4_VVER, "S%i : Lingeling-%i\n", setup.globalId, setup.diversificationIndex);
			_solver_interfaces.emplace_back(new Lingeling(setup));
			break;
		case 'c':
			// Cadical
			setup.diversificationIndex = numCdc++;
			_logger.log(V4_VVER, "S%i : Cadical-%i\n", setup.globalId, setup.diversificationIndex);
			_solver_interfaces.emplace_back(new Cadical(setup));
			break;
		/*case 'm':
			// MergeSat
			setup.diversificationIndex = numMrg++;
			_logger.log(V4_VVER, "S%i : MergeSat-%i\n", setup.globalId, setup.diversificationIndex);
			_solver_interfaces.emplace_back(new MergeSatBackend(setup));
			break;*/
#ifdef MALLOB_USE_RESTRICTED
		case 'g':
			// Glucose
			setup.diversificationIndex = numGlu++;
			_logger.log(V4_VVER, "S%i: Glucose-%i\n", setup.globalId, setup.diversificationIndex);
			_solver_interfaces.emplace_back(new MGlucose(setup));
			break;
#endif
		default:
			// Invalid solver
			_logger.log(V2_INFO, "Fatal error: Invalid solver \"%c\" assigned\n", solverChoices[cyclePos]);
			_logger.flush();
			abort();
			break;
		}
		cyclePos = (cyclePos+1) % solverChoices.size();
	}

	_sharing_manager.reset(new DefaultSharingManager(_solver_interfaces, _params, _logger));
	_logger.log(V5_DEBG, "initialized\n");
}

void HordeLib::beginSolving(size_t fSize, const int* fLits, size_t aSize, const int* aLits) {
	
	_result = UNKNOWN;

	for (size_t i = 0; i < _num_solvers; i++) {
		_solver_threads.emplace_back(new SolverThread(
			_params, _solver_interfaces[i], fSize, fLits, aSize, aLits, i, &_solution_found
		));
		_solver_threads.back()->start();
	}

	setSolvingState(ACTIVE);
	_logger.log(V5_DEBG, "started solver threads\n");
}

void HordeLib::continueSolving(size_t fSize, const int* fLits, size_t aSize, const int* aLits) {
	
	// TODO Update payload

	// unset standby
	setSolvingState(ACTIVE);
}

void HordeLib::updateRole(int rank, int numNodes) {
	
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

	// Sleeping?
    if (_sleep_microsecs > 0) usleep(_sleep_microsecs);

    // Solving done?
	bool done = false;
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		if (_solver_threads[i]->getState() == STANDBY) {
			done = true;
			_result = _solver_threads[i]->getSatResult();
			if (_result == SAT) {
				_model = _solver_threads[i]->getSolution();
			} else {
				_failed_assumptions = _solver_threads[i]->getFailedAssumptions();
			}
		}
	}

	if (done) {
		_logger.log(V5_DEBG, "Returning result\n");
		return _result;
	}
    return -1; // no result yet
}

int HordeLib::prepareSharing(int* begin, int maxSize) {
	if (isCleanedUp()) return 0;
	_logger.log(V5_DEBG, "collecting clauses on this node\n");
	return _sharing_manager->prepareSharing(begin, maxSize);
}

void HordeLib::digestSharing(std::vector<int>& result) {
	if (isCleanedUp()) return;
	_sharing_manager->digestSharing(result);
}

void HordeLib::digestSharing(int* begin, int size) {
	if (isCleanedUp()) return;
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
		dumpStats(/*final=*/true);
		setSolvingState(STANDBY);
	}
}

void HordeLib::abort() {
	if (_state != ABORTING) setSolvingState(ABORTING);
}

void HordeLib::setSolvingState(SolvingState state) {
	SolvingState oldState = _state;
	_state = state;

	_logger.log(V4_VVER, "state change %s -> %s\n", SolvingStateNames[oldState], SolvingStateNames[state]);
	for (auto& solver : _solver_threads) solver->setState(state);
}

int HordeLib::value(int lit) {
	return _model[abs(lit)];
}

int HordeLib::failed(int lit) {
	return _failed_assumptions.find(lit) != _failed_assumptions.end();
}

void HordeLib::cleanUp() {
	double time = Timer::elapsedSeconds();

	_logger.log(V5_DEBG, "[hlib-cleanup] enter\n");

	// for any running threads left:
	setSolvingState(ABORTING);
	
	// join and delete threads
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		_solver_threads[i]->tryJoin();
	}
	_solver_threads.clear();
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
