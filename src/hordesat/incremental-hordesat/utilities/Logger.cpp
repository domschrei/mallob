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

static int verbosityLevelSetting = 0;
static double start = getAbsoluteTimeLP();
static std::string identifier;

void setStartTime() {
    start = getAbsoluteTimeLP();
}

void setIdentifierString(std::string str) {
    identifier = str;
}

double getAbsoluteTimeLP() {
	timeval time;
	gettimeofday(&time, NULL);
	return (double)time.tv_sec + (double)time.tv_usec * .000001;
}

double getTime() {
	return getAbsoluteTimeLP() - start;
}

void setVerbosityLevel(int level) {
	verbosityLevelSetting = level;
}

void log(int verbosityLevel, const char* fmt ...) {
	if (verbosityLevel <= verbosityLevelSetting) {
		va_list args;
		va_start(args, fmt);
        int rank; MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		printf("[%.3f] ", getTime());
		printf("[%i] <horde-%s> ", rank, identifier.c_str());
        vprintf(fmt, args);
		va_end(args);
		fflush(stdout);
	}
}

void exitError(const char* fmt ...) {
	va_list args;
	va_start(args, fmt);
	printf("[%.3f] Exiting due to critical error: ", getTime());
	vprintf(fmt, args);
	va_end(args);
	fflush(stdout);
	exit(1);
}




