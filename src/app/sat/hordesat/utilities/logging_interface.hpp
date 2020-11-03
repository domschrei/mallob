
#ifndef HORDESAT_MALLOB_LOGGER_INTERFACE_H
#define HORDESAT_MALLOB_LOGGER_INTERFACE_H

#include <stdarg.h>
#include <memory>

class LoggingInterface {

public:
    virtual ~LoggingInterface() {};

    virtual double getTime() const = 0;

    virtual void log(int verbosityLevel, const char* fmt, ...) const = 0;
    virtual void log_va_list(int verbosityLevel, const char* fmt, va_list args) const = 0;
    virtual std::shared_ptr<LoggingInterface> copy(std::string prefix) const = 0;

    virtual void exitError(const char* fmt, ...) const = 0;
    virtual void abort() const = 0;
};

#endif