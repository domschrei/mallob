
#ifndef DOMPASCH_CONSOLE_H
#define DOMPASCH_CONSOLE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <string>

#define V0_CRIT 0
#define V1_WARN 1
#define V2_INFO 2
#define V3_VERB 3
#define V4_VVER 4
#define V5_DEBG 5
#define V6_DEBGV 6

#define LOG_ADD_DESTRANK (1<<7)
#define LOG_ADD_SRCRANK  (1<<8)
#define LOG_NO_PREFIX    (1<<9)
#define LOG_BINARYFLAGS_START   LOG_ADD_DESTRANK

//Define custom logging groups orthogonal to the verbosities 0..6 in the integer space 10..127
#define LOG_CUSTOM_CLASSES_START 10
#define V_SWEEPING 10



class Logger {

// Singleton for main console instance
private:
    static Logger _main_instance;
    Logger() {}
    Logger(const Logger& other) = delete;
    Logger& operator=(const Logger& other) = delete;
public:
    struct LoggerConfig {
        int rank {0};
        int verbosity {2};
        bool coloredOutput = false;
        bool quiet = false;
        bool cPrefix = false;
        bool flushFileImmediately = false;
        const std::string* logDirOrNull = nullptr;
        const std::string* logFilenameOrNull = nullptr;
    };

    static void init(int rank, int verbosity);
    static void init(const LoggerConfig& config);
    static Logger& getMainInstance() {
        return _main_instance;
    }
    Logger(Logger&& other);
    Logger& operator=(Logger&& other);
    ~Logger();

// Usual class members
private:
    std::string _log_directory;
    std::string _log_filename;
    std::string _line_prefix;
    FILE* _log_cfile = nullptr;
    int _rank;
    int _verbosity = 2;
    bool _colored_output = false;
    bool _quiet = false;
    bool _c_prefix = false;
    bool _flush_file_immediately = false;
    mutable pid_t _associated_tid = 0;

public:
    std::string getLogFilename() {
        return _log_filename;
    }

    std::string getLogDir(){
        return _log_directory;
    }

    void mergeJobLogs(int jobId);

    Logger copy(const std::string& linePrefix, const std::string& filenameSuffix, int verbosityOffset = 0) const;

    void log(unsigned int options, const char* str, ...) const;
    bool fail(unsigned int options, const char* str, ...) const;
    void flush() const;

    static std::string floatToStr(double num, int precision);
    static std::string dataToHexStr(const uint8_t* data, unsigned long size);

    friend void log(int options, const char* str, ...);
    friend bool log_return_false(const char* str, ...);

    void setQuiet();
    void setLinePrefix(const std::string& linePrefix);

    int getVerbosity() const;

private:

    void log(va_list& args, unsigned int options, const char* str) const;
};

void log(int options, const char* str, ...);
bool log_return_false(const char* str, ...);

void loggerCCallback(void* logger, const char* msg);

#include "logger_defs.h"

#endif
