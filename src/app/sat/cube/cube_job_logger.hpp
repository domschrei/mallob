
#ifndef MSCHICK_CUBE_JOB_LOGGER_H
#define MSCHICK_CUBE_JOB_LOGGER_H

#include <cassert>
#include <chrono>
#include <cstdlib>  // for sprintf

#include "../hordesat/utilities/logging_interface.hpp"
#include "../../../util/console.hpp"
#include "../../../util/sys/timer.hpp"
#include "../../../util/sys/threading.hpp"

class CubeJobLogger : public LoggingInterface {
   private:
    std::string _identifier;

    std::string _logfile_suffix;
    std::string _logfile_name;

    mutable std::ofstream _logfile;

    mutable Mutex _logmutex;

   public:
    CubeJobLogger(std::string identifier, std::string logfileSuffix) : _identifier(identifier), _logfile_suffix(logfileSuffix) {
        assert(logfileSuffix != "");

        _logfile_name = Console::getLogFilename();

        assert(_logfile_name != "");

        _logfile_name += _logfile_suffix;
        _logfile.open(_logfile_name);

        assert(_logfile.is_open());

        log(0, "Created logger");
    }

    ~CubeJobLogger() {
        log(0, "Destructing logger");
        // _logfile should be closed automatically
    }

    void setIdentifier(std::string identifier) {
        _identifier = identifier;
    }

    double getTime() const override {
        return Timer::elapsedSeconds();
    }

    void log(int verbosityLevel, const char *fmt, ...) const override {
        va_list args;
        va_start(args, fmt);
        log_va_list(verbosityLevel, fmt, args);
        va_end(args);
    }

    void log_va_list(int verbosityLevel, const char *fmt, va_list args) const override {
        char buf[1024];
        vsprintf(buf, fmt, args);
    
        const std::lock_guard<Mutex> lock(_logmutex);

        _logfile << getTime() << " " + _identifier + " " << std::string(buf) << endl;
    }

    std::shared_ptr<LoggingInterface> copy(std::string suffix) const override {
        return std::shared_ptr<LoggingInterface>(new CubeJobLogger(_identifier + suffix, _logfile_suffix + suffix));
    }

    void exitError(const char *fmt, ...) const override {
        va_list vl;
        va_start(vl, fmt);
        log_va_list(-2, "Exiting due to critical error:", vl);
        log_va_list(-2, fmt, vl);
        va_end(vl);
        this->abort();
    }

    void abort() const override {
        Console::log(Console::CRIT, "ERROR - aborting");
        Console::forceFlush();
        exit(1);
    }
};

#endif /* MSCHICK_CUBE_JOB_LOGGER_H */