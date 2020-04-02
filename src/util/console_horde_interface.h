
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
    void log(int verbosityLevel, const char* fmt, va_list args) {

        std::string str(fmt);
        
        // Prefix horde instance name, if not already present
        if (str.rfind("<h-", 0) != 0) {
            str = "<h-" + _identifier + "> " + str;
        }

        // Write content
        va_list argsCopy; va_copy(argsCopy, args);
        Console::log(verbosityLevel+2, str.c_str(), true, argsCopy);
        va_end(argsCopy);
    }
    void abort() {
        Console::log(Console::CRIT, "ERROR - aborting");
        Console::forceFlush();
        exit(1);
    }
};

#endif