
#ifndef DOMPASCH_CONSOLE_H
#define DOMPASCH_CONSOLE_H

#include <string>
#include <fstream>
#include <stdarg.h>
#include <mutex>

class Console {

public:
    static const int CRIT = 0;
    static const int WARN = 1;
    static const int INFO = 2;
    static const int VERB = 3;
    static const int VVERB = 4;
    static const int VVVERB = 5;
    static const int VVVVERB = 6;

private:
    static std::string logDir;
    static std::string logFilename;
    static FILE* logFile;
    static int rank;
    static int verbosity;
    static bool coloredOutput;
    static bool threadsafeOutput;
    static bool quiet;

    static bool beganLine;

    static std::mutex logMutex;

public:
    static void init(int rank, int verbosity, bool coloredOutput, bool threadsafeOutput, bool quiet, std::string logDir=".");

    static void log(int verbosity, const char* str, ...);
    static void append(int verbosity, const char* str, ...);
    static void log_send(int verbosity, int destRank, const char* str, ...);
    static void log_recv(int verbosity, int sourceRank, const char* str, ...);

    static void logUnsafe(int verbosity, const char* str, ...);
    static void appendUnsafe(int verbosity, const char* str, ...);
    static void logUnsafe(int verbosity, const char* str, bool endline, va_list& args);

    static void log(int verbosity, const char* str, bool endline, va_list& args);

    static void flush();
    static void forceFlush();

    static bool fail(const char* str, ...);

    static void getLock() {
        if (threadsafeOutput)
            logMutex.lock(); 
    };
    static void releaseLock() {
        if (threadsafeOutput)
            logMutex.unlock();
    };

    static std::string floatToStr(double num, int precision);
};

#endif