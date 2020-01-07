
#ifndef HORDE_MALLOB_DEFAULT_LOGGER_H
#define HORDE_MALLOB_DEFAULT_LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>

#include "logging_interface.h"
#include "utilities/mympi.h"

class DefaultLoggingInterface : public LoggingInterface {
private:
    int verbosity;
    std::string identifier;
    double start;

public:
    DefaultLoggingInterface() = default;
    DefaultLoggingInterface(int verbosity, std::string identifier) : verbosity(verbosity), identifier(identifier), start(getAbsoluteTimeLP()) {}

    void init(int verbosity, std::string identifier) {
        this->verbosity = verbosity;
        this->identifier = identifier;
        this->start = getAbsoluteTimeLP();
    }
    double getTime() override {
        return getAbsoluteTimeLP() - start;
    }
    void log(int verbosityLevel, const char* fmt, va_list args) override {
        if (verbosityLevel <= verbosity) {
            int rank; MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            printf("[%.3f] ", getTime());
            printf("[%i] <horde-%s> ", rank, identifier.c_str());
            vprintf(fmt, args);
            //fflush(stdout);
        }
    }
    void abort() override {
        exit(1);
    }

private:
    double getAbsoluteTimeLP() {
        timeval time;
        gettimeofday(&time, NULL);
        return (double)time.tv_sec + (double)time.tv_usec * .000001;
    }
};

#endif