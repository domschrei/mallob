
#ifndef DOMPASCH_CONSOLE_HORDE_INTERFACE_H
#define DOMPASCH_CONSOLE_HORDE_INTERFACE_H

#include "utilities/logging_interface.h"
#include "console.h"
#include "timer.h"

class ConsoleHordeInterface : public LoggingInterface {

private:
    std::string _identifier;

public:
    ConsoleHordeInterface(std::string identifier) : _identifier(identifier) {
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
        Console::log(verbosityLevel+2, str.c_str(), true, true, argsCopy);
        va_end(argsCopy);
    }
    std::shared_ptr<LoggingInterface> copy(std::string prefix) override {
        return std::shared_ptr<LoggingInterface>(new ConsoleHordeInterface(_identifier + " " + prefix));
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