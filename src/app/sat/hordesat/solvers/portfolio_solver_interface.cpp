

#include <map>
#include <chrono>

#include "util/sys/threading.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"

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
void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...) {
	std::string msg = slv->_global_name + " ";
	msg += fmt;
	va_list vl;
	va_start(vl, fmt);
	slv->_logger.log_va_list(verbosityLevel, msg.c_str(), vl);
	va_end(vl);
}

PortfolioSolverInterface::PortfolioSolverInterface(const SolverSetup& setup) 
		: _logger(*setup.logger), _setup(setup), _job_name(setup.jobname), 
		  _global_id(setup.globalId), _local_id(setup.localId), 
		  _diversification_index(setup.diversificationIndex) {
	updateTimer(_job_name);
	_global_name = "<h-" + _job_name + "_S" + std::to_string(_global_id) + ">";
}

void PortfolioSolverInterface::interrupt() {
	setSolverInterrupt();
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