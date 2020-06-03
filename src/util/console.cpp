
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
bool Console::quiet;
bool Console::cPrefix;
std::string Console::logFilename;
FILE* Console::logFile;
bool Console::beganLine;
std::mutex Console::logMutex;

void Console::init(int rank, int verbosity, bool coloredOutput, bool threadsafeOutput, bool quiet, bool cPrefix, std::string logDir) {
    Console::rank = rank;
    Console::verbosity = verbosity;
    Console::coloredOutput = coloredOutput;
    Console::threadsafeOutput = threadsafeOutput;
    Console::quiet = quiet;
    Console::cPrefix = cPrefix;
    beganLine = false;
    
    // Create logging directory as necessary
    int status = system((std::string("mkdir -p ") + logDir).c_str());
    if (status != 0) {
        log(CRIT, "ERROR while trying to create / access log directory \"%s\"", logDir.c_str());
    }

    // Open logging files
    logFilename = logDir + "/" + std::to_string(rank) + "/log" + std::string(".") + std::to_string(rank);
    logFile = fopen(logFilename.c_str(), "a");
    if (logFile == NULL) {
        log(CRIT, "ERROR while trying to open log file \"%s\"", logFilename.c_str());
    }
    /*
    if (logFile.is_open()) {
        logFile << std::fixed << std::setprecision(3);
    }
    std::cout << std::fixed << std::setprecision(3);*/
}

std::string Console::getLogFilename() {if (logFile != NULL) return logFilename; else return "";}

void Console::mergeJobLogs(int jobId) {
    std::string joblog = logFilename + "#" + std::to_string(jobId);
    std::string cmd = "cat \"" + joblog + "*\" > \"" + joblog + "_\"; mv \"" + joblog + "_\" \"job#" + std::to_string(jobId) + "\"; rm \"" + joblog + "*\"";
    Console::log(VVERB, cmd.c_str());
    int status = system(cmd.c_str());
    if (status != 0) {
        log(WARN, "WARN: Could not merge logs of job %i, exit code: %i\n", jobId, status);
    }
}

void Console::logUnsafe(int verbosity, const char* str, bool endline, bool prefix, FILE* file, va_list& args) {

    if (verbosity > Console::verbosity) return;

    // Colored output, if applicable
    if (!quiet && coloredOutput) {
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
        /*
        std::chrono::duration<double, std::micro> time_span = std::chrono::high_resolution_clock::now().time_since_epoch();
        double elapsedAbs = time_span.count();
        elapsedAbs *= 0.001f;
        elapsedAbs *= 0.001f;
        */
    
        if (prefix) {
            if (cPrefix) {
                if (!quiet) printf("c ");
                if (file != NULL) fprintf(file, "c ");
            }
            if (!quiet) printf("%.3f %i ", elapsedRel, rank);
            if (file != NULL) fprintf(file, "%.3f %i ", elapsedRel, rank);
        }
        beganLine = true;
    }

    // logging message
    va_list argsCopy; va_copy(argsCopy, args); // retrieve copy of "args"
    if (!quiet) vprintf(str, args); // consume original args
    if (file != NULL) vfprintf(file, str, argsCopy); // consume copied args
    va_end(argsCopy); // destroy copy

    // Reset terminal colors
    if (!quiet && coloredOutput) {
        std::cout << Modifier(Code::FG_DEFAULT);
    }

    // New line, if applicable
    if (endline) {
        if (strlen(str) == 0 || str[strlen(str)-1] != '\n') {
            if (!quiet) printf("\n");
            if (file != NULL) fprintf(file, "\n");
        }
        beganLine = false;
    }
}

void Console::flush() {
    if (!quiet) fflush(stdout);
    if (logFile != NULL) fflush(logFile);
}

const char BUFFER_PADDING[4096] = "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

void Console::forceFlush() {
    flush();
    log(CRIT, BUFFER_PADDING);
    flush();
}

void Console::log(int verbosity, const char* str, bool endline, bool prefix, FILE* file, va_list& args) {
    if (verbosity > Console::verbosity) return;
    getLock();
    logUnsafe(verbosity, str, endline, prefix, file, args);
    releaseLock();
}

void Console::log(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, str, true, true, logFile, vl);
    va_end(vl);
}

void Console::logUnsafe(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    logUnsafe(verbosity, str, true, true, logFile, vl);
    va_end(vl);
}

void Console::append(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, str, false, true, logFile, vl);
    va_end(vl);
}

void Console::appendUnsafe(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    logUnsafe(verbosity, str, false, true, logFile, vl);
    va_end(vl);
}

void Console::log_send(int verbosity, int destRank, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    std::string output = std::string(str) + " => [" + std::to_string(destRank) + "]";
    log(verbosity, output.c_str(), true, true, logFile, vl);
    va_end(vl);
}
void Console::log_recv(int verbosity, int sourceRank, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    std::string output = std::string(str) + " <= [" + std::to_string(sourceRank) + "]";
    log(verbosity, output.c_str(), true, true, logFile, vl);
    va_end(vl);
}

void Console::log_noprefix(int verbosity, const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(verbosity, str, true, false, logFile, vl);
    va_end(vl);
}

bool Console::fail(const char* str, ...) {
    va_list vl;
    va_start(vl, str);
    log(CRIT, str, true, true, logFile, vl);
    va_end(vl);
    return false;
}

std::string Console::floatToStr(double num, int precision) {
    std::ostringstream oss;
    oss << std::fixed;
    oss << std::setprecision(precision);
    oss << num;
    return oss.str();
}