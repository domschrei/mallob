
#include <iostream>
#include <ostream>
#include <ctime>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "console.h"
#include "timer.h"

// Taken from https://stackoverflow.com/a/17469726
enum Code {
    FG_DEFAULT = 39, 
    FG_BLACK = 30, 
    FG_RED = 31, 
    FG_GREEN = 32, 
    FG_YELLOW = 33,
    FG_BLUE = 34, 
    FG_MAGENTA = 35, 
    FG_CYAN = 36, 
    FG_LIGHT_GRAY = 37, 
    FG_DARK_GRAY = 90, 
    FG_LIGHT_RED = 91, 
    FG_LIGHT_GREEN = 92, 
    FG_LIGHT_YELLOW = 93, 
    FG_LIGHT_BLUE = 94, 
    FG_LIGHT_MAGENTA = 95, 
    FG_LIGHT_CYAN = 96, 
    FG_WHITE = 97, 
    BG_RED = 41, 
    BG_GREEN = 42, 
    BG_BLUE = 44, 
    BG_DEFAULT = 49
};
class Modifier {
    Code code;
public:
    Modifier(Code pCode) : code(pCode) {}
    friend std::ostream&
    operator<<(std::ostream& os, const Modifier& mod) {
        return os << "\033[" << mod.code << "m";
    }
};

int Console::rank;
int Console::verbosity;
bool Console::coloredOutput;
bool Console::threadsafeOutput;
std::string Console::logFilename;
FILE* Console::logFile;
bool Console::beganLine;
std::mutex Console::logMutex;

void Console::init(int rank, int verbosity, bool coloredOutput, bool threadsafeOutput, std::string logDir) {
    Console::rank = rank;
    Console::verbosity = verbosity;
    Console::coloredOutput = coloredOutput;
    Console::threadsafeOutput = threadsafeOutput;
    beganLine = false;
    
    // Create logging directory as necessary
    system((std::string("mkdir -p ") + logDir).c_str());

    // Open logging files
    logFilename = logDir + "/log_" + std::to_string(std::time(nullptr)) + std::string(".") + std::to_string(rank);
    logFile = fopen(logFilename.c_str(), "a");
    if (logFile == NULL) {
        log(CRIT, "Error while trying to open log file \"%s\"!", logFilename.c_str());
    }
    /*
    if (logFile.is_open()) {
        logFile << std::fixed << std::setprecision(3);
    }
    std::cout << std::fixed << std::setprecision(3);*/
}

void Console::logUnsafe(int verbosity, const char* str, bool endline, va_list& args) {

    if (verbosity > Console::verbosity)
        return;

    // Colored output, if applicable
    if (coloredOutput) {
        if (verbosity == Console::CRIT) {
            std::cout << Modifier(Code::FG_LIGHT_RED);
        } else if (verbosity == Console::WARN) {
            std::cout << Modifier(Code::FG_YELLOW);
        } else if (verbosity == Console::INFO) {
            std::cout << Modifier(Code::FG_WHITE);
        } else {
            std::cout << Modifier(Code::FG_LIGHT_GRAY);
        }
    }

    // Timestamp and node rank
    if (!beganLine) {
        // Relative time to program start
        float elapsedRel = Timer::elapsedSeconds();
        // Absolute time since epoch
        std::chrono::duration<double, std::micro> time_span = std::chrono::high_resolution_clock::now().time_since_epoch();
        double elapsedAbs = time_span.count();
        elapsedAbs *= 0.001f;
        elapsedAbs *= 0.001f;
        
        printf("[%.6f / %.6f] [%i] ", elapsedAbs, elapsedRel, rank);
        if (logFile != NULL) fprintf(logFile, "[%.6f / %.6f] [%i] ", elapsedAbs, elapsedRel, rank);
        
        beganLine = true;
    }

    // logging message
    va_list argsCopy; va_copy(argsCopy, args); // retrieve copy of "args"
    vprintf(str, args); // consume original args
    if (logFile != NULL) {
        vfprintf(logFile, str, argsCopy); // consume copied args
    }
    va_end(argsCopy); // destroy copy

    // Reset terminal colors
    if (coloredOutput) {
        std::cout << Modifier(Code::FG_DEFAULT);
    }

    // New line, if applicable
    if (endline) {
        if (strlen(str) == 0 || str[strlen(str)-1] != '\n') {
            printf("\n");
            if (logFile != NULL) fprintf(logFile, "\n");
        }
        beganLine = false;
    }
}

void Console::flush() {
    fflush(stdout);
    if (logFile != NULL) fflush(logFile);
}

void Console::log(int verbosity, const char* str, bool endline, va_list& args) {
    getLock();
    logUnsafe(verbosity, str, endline, args);
    releaseLock();
}

void Console::log(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, str, true, vl);
    va_end(vl);
}

void Console::logUnsafe(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    logUnsafe(verbosity, str, true, vl);
    va_end(vl);
}

void Console::append(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, str, false, vl);
    va_end(vl);
}

void Console::appendUnsafe(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    logUnsafe(verbosity, str, false, vl);
    va_end(vl);
}

void Console::log_send(int verbosity, int destRank, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, (std::string(str) + " => [" + std::to_string(destRank) + "]").c_str(), true, vl);
    va_end(vl);
}
void Console::log_recv(int verbosity, int sourceRank, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, (std::string(str) + " <= [" + std::to_string(sourceRank) + "]").c_str(), true, vl);
    va_end(vl);
}

bool Console::fail(const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(CRIT, str, true, vl);
    va_end(vl);
    return false;
}