
#include <map>
#include <chrono>
#include <atomic>
#include "util/assert.hpp"

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
		  _import_buffer(setup, _stats) {
	updateTimer(_job_name);
	_global_name = "<h-" + _job_name + "_S" + std::to_string(_global_id) + ">";
	_stats.histProduced = new ClauseHistogram(setup.strictClauseLengthLimit);
	_stats.histDigested = new ClauseHistogram(setup.strictClauseLengthLimit);

	LOGGER(_logger, V4_VVER, "Diversification index %i\n", getDiversificationIndex());
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
	_import_buffer.add(c);
}

int PortfolioSolverInterface::getClauseImportBudget(int clauseLength, int lbd) {
	if (_clause_sharing_disabled) return 0;
	return _import_buffer.getLiteralBudget(clauseLength, lbd);
}

bool PortfolioSolverInterface::fetchLearnedClause(Mallob::Clause& clauseOut, AdaptiveClauseDatabase::ExportMode mode) {
	if (_clause_sharing_disabled) return false;
	clauseOut = _import_buffer.get(mode);
	return clauseOut.begin != nullptr && clauseOut.size >= 1;
}

std::vector<int> PortfolioSolverInterface::fetchLearnedUnitClauses() {
	if (_clause_sharing_disabled) return std::vector<int>();
	return _import_buffer.getUnitsBuffer();
}
