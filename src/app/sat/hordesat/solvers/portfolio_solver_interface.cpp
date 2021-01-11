

#include <map>
#include <chrono>

#include "util/sys/threading.hpp"
#include "util/logger.hpp"

#include "portfolio_solver_interface.hpp"

using namespace std::chrono;

Mutex timeCallbackLock;
std::map<std::string, high_resolution_clock::time_point> times;
std::string currentSolverName = "";
high_resolution_clock::time_point lglSolverStartTime;

void updateTimer(std::string jobName) {
	auto lock = timeCallbackLock.getLock();
	if (currentSolverName == jobName) return;
	if (!times.count(jobName)) {
		times[jobName] = high_resolution_clock::now();
	}
	lglSolverStartTime = times[jobName];
	currentSolverName = jobName;
}
double getTime() {
    high_resolution_clock::time_point nowTime = high_resolution_clock::now();
	timeCallbackLock.lock();
    duration<double, std::milli> time_span = nowTime - lglSolverStartTime;    
	timeCallbackLock.unlock();
	return time_span.count() / 1000;
}

PortfolioSolverInterface::PortfolioSolverInterface(const SolverSetup& setup) 
		: _logger(setup.logger->copy("S"+std::to_string(setup.globalId), "S"+std::to_string(setup.globalId))), 
		  _setup(setup), _job_name(setup.jobname), 
		  _global_id(setup.globalId), _local_id(setup.localId), 
		  _diversification_index(setup.diversificationIndex) {
	updateTimer(_job_name);
	_global_name = "<h-" + _job_name + "_S" + std::to_string(_global_id) + ">";
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