
#include <map>
#include <chrono>
#include <atomic>
#include "app/sat/sharing/adaptive_import_manager.hpp"
#include "app/sat/sharing/ring_buffer_import_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/assert.hpp"

#include "util/random.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

#include "portfolio_solver_interface.hpp"

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
	_stats.histProduced = new ClauseHistogram(setup.strictClauseLengthLimit);
	_stats.histDigested = new ClauseHistogram(setup.strictClauseLengthLimit);

	LOGGER(_logger, V4_VVER, "Diversification index %i\n", getDiversificationIndex());

	// Set manipulator for non-unit clauses just before they are handed to the solver.
	_import_manager->setPreimportClauseManipulator([this](Mallob::Clause& c) {
		if (!c.begin) return; // no clause
		if (c.size == 1) return; // unit clause
		if (_setup.randomizeLbdBeforeImport) {
			// Workaround to get "uniform" drawing of numbers from [2, c.size]
			c.lbd = (int) std::round(_rng.randomInRange(2 - 0.49999, c.size + 0.49999));
			assert(c.lbd >= 2);
			assert(c.lbd <= c.size);
		}
		if (_setup.incrementLbdBeforeImport && c.lbd < c.size)
			c.lbd++;
		if (_setup.resetLbdBeforeImport) c.lbd = c.size;
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
	setLearnedClauseCallback([callback, this](const Mallob::Clause& c, int solverId) {
		if (_terminated || _clause_sharing_disabled) return;
		int condVar = _current_cond_var_or_zero;
		assert(condVar >= 0);
		callback(c, solverId, getSolverSetup().solverRevision, condVar);
	});
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
