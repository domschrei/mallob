
#include <assert.h>
#include <bits/chrono.h>
#include <map>
#include <atomic>
#include <cmath>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/sharing/adaptive_import_manager.hpp"
#include "app/sat/sharing/ring_buffer_import_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
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
		  _setup(setup), _job_name(setup.jobname), 
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
		// Does this thread in particular run in certified UNSAT mode?
		if (_setup.onTheFlyChecking) {
			// Yes: ALWAYS create your own LRAT connector. Have it support checking of models
			// ONLY IF desired and there is no pre-created LRATConnector instance for this purpose.
			LOGGER(_logger, V3_VERB, "Creating full LratConnector%s\n", createModelCheckingLratConn?" with checking models":"");
			_lrat = new LratConnector(_logger, _setup.localId, _setup.numVars,
				createModelCheckingLratConn
			);
			if (createModelCheckingLratConn) _setup.modelCheckingLratConnector = _lrat;
		} else {
			// No: only create a model-checking LRATConnector if it is not precreated yet.
			if (createModelCheckingLratConn) {
				LOGGER(_logger, V3_VERB, "Creating dedicated LratConnector for checking models\n");
				_setup.modelCheckingLratConnector = new LratConnector(
					_logger, _setup.localId, _setup.numVars, true
				);
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
		if (_setup.randomizeLbdBeforeImport) {
			// Workaround to get "uniform" drawing of numbers from [2, c.size]
			c.lbd = (int) std::round(_rng.randomInRange(2 - 0.49999, c.size-ClauseMetadata::numInts() + 0.49999));
			assert(c.lbd >= 2);
			assert(c.lbd <= c.size-ClauseMetadata::numInts());
		}
		if (_setup.incrementLbdBeforeImport && c.lbd < c.size-ClauseMetadata::numInts())
			c.lbd++;
		if (_setup.resetLbdBeforeImport) c.lbd = c.size-ClauseMetadata::numInts();
	});
}

void PortfolioSolverInterface::interrupt() {
	setSolverInterrupt();
	_logger.flush();
}
void PortfolioSolverInterface::uninterrupt() {
	updateTimer(_job_name);
	unsetSolverInterrupt();
}
void PortfolioSolverInterface::suspend() {
	setSolverSuspend();
}
void PortfolioSolverInterface::resume() {
	updateTimer(_job_name);
	unsetSolverSuspend();
}
void PortfolioSolverInterface::setTerminate() {
	_terminated = true;
	interrupt();
}

void PortfolioSolverInterface::setExtLearnedClauseCallback(const ExtLearnedClauseCallback& callback) {
	auto cb = ([callback, this](const Mallob::Clause& c, int solverId) {
		if (_terminated || !_setup.exportClauses) return;
		int condVar = _current_cond_var_or_zero;
		assert(condVar >= 0);
		callback(c, solverId, getSolverSetup().solverRevision, condVar);
	});
	setLearnedClauseCallback(cb);
	if (_lrat) _lrat->setLearnedClauseCallback(cb);
}

void PortfolioSolverInterface::setExtProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) {
	setProbingLearnedClauseCallback(callback);
	if (_lrat) _lrat->setProbingLearnedClauseCallback(callback);
}

void PortfolioSolverInterface::addLearnedClause(const Mallob::Clause& c) {
	if (_clause_sharing_disabled) return;
	_import_manager->addSingleClause(c);
}

bool PortfolioSolverInterface::fetchLearnedClause(Mallob::Clause& clauseOut, GenericClauseStore::ExportMode mode) {
	if (_clause_sharing_disabled) return false;
	clauseOut = _import_manager->getClause(mode);
	return clauseOut.begin != nullptr && clauseOut.size >= 1;
}

std::vector<int> PortfolioSolverInterface::fetchLearnedUnitClauses() {
	if (_clause_sharing_disabled) return std::vector<int>();
	return _import_manager->getUnitsBuffer();
}

PortfolioSolverInterface::~PortfolioSolverInterface() {
	if (_lrat) delete _lrat;
	if (_setup.owningModelCheckingLratConnector) delete _setup.modelCheckingLratConnector;
}
