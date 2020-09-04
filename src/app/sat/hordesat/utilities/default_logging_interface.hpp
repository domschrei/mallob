
#ifndef HORDE_MALLOB_DEFAULT_LOGGER_H
#define HORDE_MALLOB_DEFAULT_LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include <string>

// Turn off incompatible function types warning in openmpi
#define OMPI_SKIP_MPICXX 1
#include <mpi.h>

#include "logging_interface.hpp"

class DefaultLoggingInterface : public LoggingInterface {
private:
    int verbosity = 0;
    std::string identifier = "";
    double start = 0;

public:
    DefaultLoggingInterface() = default;
    DefaultLoggingInterface(int verbosity, std::string identifier) : verbosity(verbosity), identifier(identifier), start(getAbsoluteTimeLP()) {}

    void init(int verbosity, std::string identifier) {
        this->verbosity = verbosity;
        this->identifier = identifier;
        this->start = getAbsoluteTimeLP();
    }
    double getTime() const override {
        return getAbsoluteTimeLP() - start;
    }

    void log(int verbosityLevel, const char* fmt, ...) const override {
        va_list vl;
        va_start(vl, fmt);
        log_va_list(verbosityLevel, fmt, vl);
        va_end(vl);
    }
    void log_va_list(int verbosityLevel, const char* fmt, va_list args) const override {
        if (verbosityLevel <= verbosity) {
            int rank; MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            printf("[%.3f] ", getTime());
            printf("[%i] %s ", rank, identifier.c_str());
            vprintf(fmt, args);
            //fflush(stdout);
        }
    }
    std::shared_ptr<LoggingInterface> copy(std::string prefix) const override {
        return std::shared_ptr<LoggingInterface>(new DefaultLoggingInterface(verbosity, identifier + " " + prefix));
    }

    void exitError(const char* fmt, ...) const override {
        va_list vl;
        va_start(vl, fmt);
        log_va_list(-1, "Exiting due to critical error:", vl);
        log_va_list(-1, fmt, vl);
        va_end(vl);
        this->abort();
    }
    void abort() const override {
        exit(1);
    }

private:
    double getAbsoluteTimeLP() const {
        timeval time;
        gettimeofday(&time, NULL);
        return (double)time.tv_sec + (double)time.tv_usec * .000001;
    }
};

#endif