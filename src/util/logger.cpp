
#include "logger.hpp"

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
