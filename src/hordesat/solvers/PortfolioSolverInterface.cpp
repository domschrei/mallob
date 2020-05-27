

#include <map>
#include <chrono>

#include "../utilities/Threading.h"
#include "../utilities/logging_interface.h"

#include "PortfolioSolverInterface.h"

using namespace std::chrono;

Mutex timeCallbackLock;
std::map<std::string, high_resolution_clock::time_point> times;
std::string currentSolverName = "";
high_resolution_clock::time_point lglSolverStartTime;

void updateTimer(std::string solverName) {
	auto lock = timeCallbackLock.getLock();
	if (currentSolverName == solverName) return;
	if (!times.count(solverName)) {
		times[solverName] = high_resolution_clock::now();
	}
	lglSolverStartTime = times[solverName];
	currentSolverName = solverName;
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