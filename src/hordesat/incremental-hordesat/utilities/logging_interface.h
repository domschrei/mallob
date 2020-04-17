
#ifndef HORDESAT_MALLOB_LOGGER_INTERFACE_H
#define HORDESAT_MALLOB_LOGGER_INTERFACE_H

#include <stdarg.h>

class LoggingInterface {

public:
    virtual double getTime() = 0;
    virtual void log(int verbosityLevel, const char* fmt, ...) = 0;
    virtual void log_va_list(int verbosityLevel, const char* fmt, va_list args) = 0;
    virtual void exitError(const char* fmt, ...) = 0;
    virtual void abort() = 0;
};

#endif