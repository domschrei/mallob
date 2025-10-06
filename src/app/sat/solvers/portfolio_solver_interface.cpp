
#include <assert.h>
#include <map>
#include <atomic>
#include <cmath>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/sharing/adaptive_import_manager.hpp"
#include "app/sat/sharing/ring_buffer_import_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/solvers/solving_replay.hpp"
#include "util/random.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "portfolio_solver_interface.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_histogram.hpp"

using namespace std::chrono;

Mutex timeCallbackLock;
std::map<std::string, unsigned long> times;
std::string currentSolverName = "";
std::atomic_ulong lglSolverStartTime;

void updateTimer(std::string jobName) {
	auto lock = timeCallbackLock.getLock();
	if (currentSolverName == jobName) return;
	if (!times.count(jobName)) {
		times[jobName] = (unsigned long) (1000*1000*Timer::elapsedSeconds());
	}
	lglSolverStartTime = times[jobName];
	currentSolverName = jobName;
}
double getTime() {
    auto nowTime = (unsigned long) (1000*1000*Timer::elapsedSeconds());
    double timeSpan = nowTime - lglSolverStartTime;    
	return ((double)timeSpan) / (1000 * 1000);
}

PortfolioSolverInterface::PortfolioSolverInterface(const SolverSetup& setup) 
		: _logger(setup.logger->copy(
				"S"+std::to_string(setup.globalId)+"."+std::to_string(setup.solverRevision), 
				"." + setup.jobname + ".S"+std::to_string(setup.globalId)
		  )), 
		  _setup(setup),
		  _replay(_setup.replayMode, "replay." + std::to_string(setup.globalId)),
		  _job_name(setup.jobname),
		  _global_id(setup.globalId), _local_id(setup.localId), 
		  _diversification_index(setup.diversificationIndex),
		  _import_manager([&]() -> GenericImportManager* {
			if (setup.adaptiveImportManager) {
				return new AdaptiveImportManager(setup, _stats);
			} else {
				return new RingBufferImportManager(setup, _stats);
			}
		  }()), _rng(_setup.globalId) {
	updateTimer(_job_name);
	_global_name = "<h-" + _job_name + "_S" + std::to_string(_global_id) + ">";
	_stats.histProduced = new ClauseHistogram(setup.strictMaxLitsPerClause+ClauseMetadata::numInts());
	_stats.histDigested = new ClauseHistogram(setup.strictMaxLitsPerClause+ClauseMetadata::numInts());

	// Is on-the-fly checking generally enabled?
	if (_setup.onTheFlyChecking || _setup.onTheFlyCheckModel) {
		// Yes. Is this the thread that needs to create *the* model-checking LRAT connector?
		bool createModelCheckingLratConn = _setup.onTheFlyCheckModel && !_setup.modelCheckingLratConnector;
		TrustedCheckerProcessAdapter::TrustedCheckerProcessSetup chkSetup {
			_logger, _setup.baseSeed, _setup.jobId,
			_setup.globalId, _setup.localId, true
		};
		// Does this thread in particular run in certified UNSAT mode?
		if (_setup.onTheFlyChecking) {
			// Yes: ALWAYS create your own LRAT connector. Have it support checking of models
			// ONLY IF desired and there is no pre-created LRATConnector instance for this purpose.
			LOGGER(_logger, V3_VERB, "Creating full LratConnector%s\n", createModelCheckingLratConn?" with checking models":"");
			chkSetup.checkModel = createModelCheckingLratConn;
			_lrat = new LratConnector(chkSetup);
			if (createModelCheckingLratConn) _setup.modelCheckingLratConnector = _lrat;
		} else {
			// No: only create a model-checking LRATConnector if it is not precreated yet.
			if (createModelCheckingLratConn) {
				LOGGER(_logger, V3_VERB, "Creating dedicated LratConnector for checking models\n");
				_setup.modelCheckingLratConnector = new LratConnector(chkSetup);
				_setup.owningModelCheckingLratConnector = true;
			}
		}
		// if (createModelCheckingLratConn), the next solvers on this process will be given
		// the LRATConnector that was just created, for the purpose of checking any found model.
	}

	LOGGER(_logger, V4_VVER, "Diversification index %i\n", getDiversificationIndex());

	// Set manipulator for incoming clauses just before they are handed to the solver.
	_import_manager->setPreimportClauseManipulator([this](Mallob::Clause& c) {
		if (!c.begin) return; // no clause
		if (c.size == 1 || c.size-ClauseMetadata::numInts() == 1) {
			// unit clause (perhaps with metadata)
			c.lbd = 1;
			return;
		}
		if (_setup.randomizeLbdBeforeImport==1) {
			//Uniform sample
			// Workaround to get "uniform" drawing of numbers from [2, c.size]
			c.lbd = (int) std::round(_rng.randomInRange(2 - 0.49999, c.size-ClauseMetadata::numInts() + 0.49999));
			assert(c.lbd >= 2);
			assert(c.lbd <= c.size-ClauseMetadata::numInts());
		}
		else if (_setup.randomizeLbdBeforeImport==2) {
			//Triangle sample
			int truesize = c.size - ClauseMetadata::numInts();
			//Special cases:
			//Size=1: Unit clause, want to keep lbd=1
			//Size=2: Set lbd=2, otherwise lbd=3 would cause assertion failure
			//Size=3: Set lbd=3, because we want to bump all lbds (if possible) to at least 3
			if(truesize<=3) {
				c.lbd=truesize;
			}
			else {
				//Sizes >=4: Choose a random lbd from [3,..,size], sapled form a triangular distribution, i.e. higher lbds are more likely
				int lbd_options= std::max(1,truesize-2);
				//For larger clauses we use a triangular distribution to more weight to higher lbd values
				//We sample from a rectangle by stacking two triangles above each other (one flippe)
				// lbd_start: the "column" of the rectangle, range [0,lbd_options-1]
				// flip:      the "row" of the rectangle,    range [0,lbd_option]
				//if flip lands in the "flipped" triangle region, we mirror the lbd around the center
				//This works because each pair of columns (mirrored around the center) have the same summed weight
				int lbd_column = (int) std::round(_rng.randomInRange(0 - 0.49999,(lbd_options-1) + 0.49999));
				int flip = (int) std::round(_rng.randomInRange(0 - 0.49999, lbd_options    + 0.49999));
				//A large enough j "mirrors" the lbd to the other end of the triangle distribution
				if(lbd_column <= flip) lbd_column = (lbd_options-1) - lbd_column;
				//Our distribution was calculated the range [0,...] so now we transform it into [3,...] for have always lbd=>3
				c.lbd = lbd_column+3;
				//cout << "eff_size="<<(c.size-ClauseMetadata::numInts())<<endl;
				//cout <<" trig_lbd="<<c.lbd<<endl;
			}
		}

		if (_setup.incrementLbdBeforeImport && c.lbd < c.size-ClauseMetadata::numInts())
			c.lbd++;
		if (_setup.resetLbdBeforeImport) c.lbd = c.size-ClauseMetadata::numInts();



	});

	if (!_setup.objectiveFunction.empty()) {
		_optimizer.reset(new OptimizingPropagator(_setup.objectiveFunction, _setup.numVars));
	}
}

void PortfolioSolverInterface::interrupt() {
	setSolverInterrupt();
	_logger.flush();
}
void PortfolioSolverInterface::uninterrupt() {
	updateTimer(_job_name);
	unsetSolverInterrupt();
}
void PortfolioSolverInterface::setTerminate() {
	_terminated = true;
	interrupt();
}

void PortfolioSolverInterface::setExtLearnedClauseCallback(const ExtLearnedClauseCallback& callback) {
	auto cb = ([callback, this](const Mallob::Clause& c, int solverId) {
		if (_terminated || !_setup.exportClauses) return;
		callback(c, solverId, getSolverSetup().solverRevision, _conditional_lits);
	});
	setLearnedClauseCallback(cb);
	if (_lrat) _lrat->setLearnedClauseCallback(cb);
}

void PortfolioSolverInterface::setExtProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) {
	setProbingLearnedClauseCallback(callback);
	if (_lrat) _lrat->setProbingLearnedClauseCallback(callback);
}

void PortfolioSolverInterface::addLearnedClause(const Mallob::Clause& c) {
	if (!_clause_import_enabled) return;
	_import_manager->addSingleClause(c);
}

bool PortfolioSolverInterface::fetchLearnedClause(Mallob::Clause& clauseOut, GenericClauseStore::ExportMode mode) {
	if (_replay.getMode() == SolvingReplay::REPLAY)
		return _replay.replayImportCallback(clauseOut, mode);
	bool success = false;
	if (_clause_import_enabled) {
		clauseOut = _import_manager->getClause(mode);
		success = clauseOut.begin != nullptr;
	}
	if (_replay.getMode() == SolvingReplay::RECORD)
		_replay.recordImportCallback(success, clauseOut, mode);
	return success;
}

std::vector<int> PortfolioSolverInterface::fetchLearnedUnitClauses() {
	if (_replay.getMode() == SolvingReplay::REPLAY)
		return _replay.replayImportCallback();
	std::vector<int> units;
	if (_clause_import_enabled) {
		units = _import_manager->getUnitsBuffer();
	}
	if (_replay.getMode() == SolvingReplay::RECORD) {
		_replay.recordImportCallback(units);
	}
	return units;
}

PortfolioSolverInterface::~PortfolioSolverInterface() {
	if (_lrat) delete _lrat;
	if (_setup.owningModelCheckingLratConnector) delete _setup.modelCheckingLratConnector;
}
