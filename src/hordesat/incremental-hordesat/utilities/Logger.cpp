/*
 * Logger.cpp
 *
 *  Created on: Mar 9, 2015
 *      Author: balyo
 */

#include "Logger.h"
#include "mympi.h"

#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static std::shared_ptr<LoggingInterface> loggingInterface;

void setLoggingInterface(std::shared_ptr<LoggingInterface> interf) {
	loggingInterface = interf;
}

double getTime() {
	return loggingInterface->getTime();
}

void log(int verbosityLevel, const char* fmt, ...) {
	va_list vl;
	va_start(vl, fmt);
	loggingInterface->log(verbosityLevel, fmt, vl);
	va_end(vl);
}

void log_va_list(int verbosityLevel, const char* fmt, va_list vl) {
	va_list copy;
	va_copy(copy, vl);
	loggingInterface->log(verbosityLevel, fmt, copy);
	va_end(copy);
}

void exitError(const char* fmt, ...) {
	va_list vl;
	va_start(vl, fmt);
	loggingInterface->log(-1, "Exiting due to critical error:", vl);
	loggingInterface->log(-1, fmt, vl);
	va_end(vl);
	loggingInterface->abort();
}




