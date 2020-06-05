
#ifndef DOMPASCH_CONSOLE_HORDE_INTERFACE_H
#define DOMPASCH_CONSOLE_HORDE_INTERFACE_H

#include "hordesat/utilities/logging_interface.hpp"
#include "util/console.hpp"
#include "util/sys/timer.hpp"

class ConsoleHordeInterface : public LoggingInterface {

private:
    std::string _identifier;

    std::string _logfile_suffix;
    std::string _logfile_name;
    FILE* _logfile = NULL;

public:
    ConsoleHordeInterface(std::string identifier, std::string logfileSuffix) : _identifier(identifier), _logfile_suffix(logfileSuffix) {
        _logfile_name = Console::getLogFilename();
        if (_logfile_name.size() > 0) {
            _logfile_name += _logfile_suffix;
            _logfile = fopen(_logfile_name.c_str(), "a");
            if (_logfile == NULL) {
                log(Console::CRIT, "ERROR while trying to open log file \"%s\"", _logfile_name.c_str());
            }
        }
    }
    ~ConsoleHordeInterface() {
        if (_logfile != NULL) fclose(_logfile);
    }

    double getTime() {
        return Timer::elapsedSeconds();
    }

    void log(int verbosityLevel, const char* fmt, ...) override {
        va_list args;
        va_start(args, fmt);
        log_va_list(verbosityLevel, fmt, args);
        va_end(args);
    }
    void log_va_list(int verbosityLevel, const char* fmt, va_list args) override {

        std::string str(fmt);
        
        // Prefix horde instance name, if not already present
        if (str.rfind(_identifier, 0) != 0) {
            str = _identifier + " " + str;
        }

        // Write content
        va_list argsCopy; va_copy(argsCopy, args);
        Console::log(verbosityLevel+2, str.c_str(), true, true, _logfile, argsCopy);
        va_end(argsCopy);
    }
    std::shared_ptr<LoggingInterface> copy(std::string suffix) override {
        return std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(_identifier + suffix, _logfile_suffix + suffix));
    }

    void exitError(const char* fmt, ...) override {
        va_list vl;
        va_start(vl, fmt);
        log_va_list(-2, "Exiting due to critical error:", vl);
        log_va_list(-2, fmt, vl);
        va_end(vl);
        this->abort();
    }
    void abort() override {
        Console::log(Console::CRIT, "ERROR - aborting");
        Console::forceFlush();
        exit(1);
    }
};

#endif