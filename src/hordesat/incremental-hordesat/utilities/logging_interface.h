
#ifndef HORDESAT_MALLOB_LOGGER_INTERFACE_H
#define HORDESAT_MALLOB_LOGGER_INTERFACE_H

#include <stdarg.h>

class LoggingInterface {

public:
    virtual double getTime() = 0;
    virtual void log(int verbosityLevel, const char* fmt, va_list args) = 0;
    virtual void abort() = 0;
};

#endif