
#include <iostream>
#include <ostream>
#include <ctime>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "util/sys/fileutils.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/proc.hpp"

#include "logger.hpp"

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

Logger Logger::_main_instance;

void log(int options, const char* str, ...) {
    va_list args;
    va_start(args, str);
    Logger::getMainInstance().log(args, options, str);
    va_end(args);
}

bool log_return_false(const char* str, ...) {
    va_list args;
    va_start(args, str);
    Logger::getMainInstance().log(args, V0_CRIT, str);
    va_end(args);
    return false;
}

void Logger::init(int rank, int verbosity) {
    LoggerConfig config;
    config.rank = rank;
    config.verbosity = verbosity;
    Logger::init(config);
}

void Logger::init(const LoggerConfig& config) {
    _main_instance._rank = config.rank;
    _main_instance._verbosity = std::min(7, config.verbosity);
    _main_instance._colored_output = config.coloredOutput;
    _main_instance._quiet = config.quiet;
    _main_instance._c_prefix = config.cPrefix;
    _main_instance._flush_file_immediately = config.flushFileImmediately;

    // Create logging directory as necessary
    if (config.logDirOrNull != nullptr && config.logFilenameOrNull != nullptr) {
        const std::string& logDir = *config.logDirOrNull;

        _main_instance._log_directory = (logDir.size() == 0 ? "." : logDir) + "/" + std::to_string(config.rank);
        bool dirExists = FileUtils::isDirectory(_main_instance._log_directory);
        _main_instance._log_directory += "/";
        if (!dirExists) {
            int status = FileUtils::mkdir(_main_instance._log_directory);
            if (status != 0) {
                _main_instance._log_cfile = nullptr;
                _main_instance._quiet = false;
                LOGGER(_main_instance, V0_CRIT, "[ERROR] status %i while trying to create / access log directory \"%s\"\n", 
                    status, _main_instance._log_directory.c_str());
                abort();
            }
        }

        // Open logging files
        _main_instance._log_filename = _main_instance._log_directory + *config.logFilenameOrNull;
        _main_instance._log_cfile = fopen(_main_instance._log_filename.c_str(), "a");
        if (_main_instance._log_cfile == nullptr) {
            _main_instance._quiet = false;
            LOGGER(_main_instance, V0_CRIT, "[ERROR] cannot open log file \"%s\", errno=%i\n", 
                _main_instance._log_filename.c_str(), errno);
            abort();
        }
    }
}
Logger::Logger(Logger&& other) :
    _log_directory(std::move(other._log_directory)), _log_filename(std::move(other._log_filename)), 
    _line_prefix(std::move(other._line_prefix)), _log_cfile(other._log_cfile), _rank(other._rank), 
    _verbosity(other._verbosity), _colored_output(other._colored_output), _quiet(other._quiet), 
    _c_prefix(other._c_prefix), _flush_file_immediately(other._flush_file_immediately) {
    
    other._log_cfile = nullptr;
}
Logger& Logger::operator=(Logger&& other) {
    _log_directory = std::move(other._log_directory);
    _log_filename = std::move(other._log_filename);
    _line_prefix = std::move(other._line_prefix);
    _log_cfile = other._log_cfile;
    _rank = other._rank;
    _verbosity = other._verbosity;
    _colored_output = other._colored_output;
    _quiet = other._quiet;
    _c_prefix = other._c_prefix;
    _flush_file_immediately = other._flush_file_immediately;

    other._log_cfile = nullptr;
    return *this;
}
Logger::~Logger() {
    flush();
    if (_log_cfile != nullptr) fclose(_log_cfile);
}

void Logger::mergeJobLogs(int jobId) {
    std::string globstring = _log_directory + "*.#" + std::to_string(jobId) + ".S*";
    std::string dest = _log_directory + "jobs." + std::to_string(_rank);
    int status = FileUtils::mergeFiles(globstring, dest, true);
    if (status != 0) {
        LOG(V1_WARN, "[WARN] Could not merge logs, exit code: %i\n", status);
    }
}

Logger Logger::copy(const std::string& linePrefix, const std::string& filenameSuffix, int verbosityOffset) const {
    Logger c;
    if (_log_cfile != nullptr) {
        c._log_directory = _log_directory;
        c._log_filename = _log_filename + filenameSuffix;
        c._log_cfile = fopen(c._log_filename.c_str(), "a");
        if (c._log_cfile == nullptr) {
            c._quiet = false;
            LOG(V0_CRIT, "[ERROR] cannot open child log file \"%s\", errno=%i\n", c._log_filename.c_str(), errno);
            abort();
        }
    }
    c._line_prefix = _line_prefix + " " + linePrefix;
    c._verbosity = _verbosity + verbosityOffset;
    c._rank = _rank;
    c._colored_output = _colored_output;
    c._quiet = _quiet;
    c._c_prefix = _c_prefix;
    c._flush_file_immediately = _flush_file_immediately;
    return c;
}

void Logger::setQuiet() {
    _quiet = true;
}

void Logger::setLinePrefix(const std::string& linePrefix) {
    _line_prefix = linePrefix;
}

void Logger::log(unsigned int options, const char* str, ...) const {
    va_list args;
    va_start(args, str);
    log(args, options, str);
    va_end(args);
}

bool Logger::fail(unsigned int options, const char* str, ...) const {
    va_list args;
    va_start(args, str);
    log(args, options, str);
    va_end(args);
    return false;
}

void Logger::flush() const {
    if (!_quiet) fflush(stdout);
    if (_log_cfile != nullptr) fflush(_log_cfile);
}

std::string Logger::floatToStr(double num, int precision) {
    std::ostringstream oss;
    oss << std::fixed;
    oss << std::setprecision(precision);
    oss << num;
    return oss.str();
}

void Logger::log(va_list& args, unsigned int options, const char* str) const {
        
    /*
    // Abort on unsafe logging concurrency
    if (_associated_tid == 0) {
        _associated_tid = Proc::getTid();
    } else if (_associated_tid > 0) {
        auto tid = Proc::getTid();
        if (tid != _associated_tid) {
            auto prevTid = _associated_tid;
            _associated_tid = -1;
            LOG(V0_CRIT, "[ERROR] Unsafe concurrency in %s (original tid: %ld, this tid: %ld)\n", 
                    _log_filename.c_str(), prevTid, tid);
            fflush(stdout);
            abort();
        }
    }
    */

    int verbosity = options & 7;
    if (verbosity > _verbosity) return;
    bool prefix = (options & LOG_NO_PREFIX) == 0;
    bool withDestRank = (options & LOG_ADD_DESTRANK) != 0;
    bool withSrcRank = (options & LOG_ADD_SRCRANK) != 0;
    
    int otherRank = -1;
    if (withDestRank || withSrcRank) {
        otherRank = va_arg(args, int);
    }

    // Colored output, if applicable
    if (!_quiet && _colored_output) {
        if (verbosity <= V0_CRIT) {
            std::cout << Modifier(Code::FG_LIGHT_CYAN);
        } else if (verbosity == V1_WARN) {
            std::cout << Modifier(Code::FG_CYAN);
        } else if (verbosity == V2_INFO) {
            std::cout << Modifier(Code::FG_LIGHT_BLUE);
        } else if (verbosity == V3_VERB) {
            std::cout << Modifier(Code::FG_BLUE);
        } else if (verbosity == V4_VVER) {
            std::cout << Modifier(Code::FG_MAGENTA);
        } else if (verbosity >= V5_DEBG) {
            std::cout << Modifier(Code::FG_DARK_GRAY);
        }
    }

    // Timestamp and node rank
    if (prefix) {
        // Relative time to program start
        float elapsedRel = Timer::elapsedSeconds();
        if (_c_prefix) {
            if (!_quiet) printf("c ");
            if (_log_cfile != nullptr) fprintf(_log_cfile, "c ");
        }
        if (!_quiet) printf("%.3f %i%s ", elapsedRel, _rank, _line_prefix.c_str());
        if (_log_cfile != nullptr) {
            fprintf(_log_cfile, "%.3f %i%s ", elapsedRel, _rank, _line_prefix.c_str());
        }
    }

    // logging message
    va_list argsCopy; va_copy(argsCopy, args); // retrieve copy of "args"
    if (!_quiet) vprintf(str, args); // consume original args
    if (_log_cfile != nullptr) vfprintf(_log_cfile, str, argsCopy); // consume copied args
    if (otherRank >= 0) {
        auto arrowStr = withDestRank ? "=>" : "<=";
        if (!_quiet) printf(" %s [%i]\n", arrowStr, otherRank);
        if (_log_cfile != nullptr) {
            fprintf(_log_cfile, " %s [%i]\n", arrowStr, otherRank);
        }
    }
    va_end(argsCopy); // destroy copy
    
    // Immediate file flushing if desired
    if (_log_cfile != nullptr && _flush_file_immediately) fflush(_log_cfile);
    
    // Reset terminal colors
    if (!_quiet && _colored_output) {
        std::cout << Modifier(Code::FG_DEFAULT);
    }
}

int Logger::getVerbosity() const {
    return _verbosity;
}
