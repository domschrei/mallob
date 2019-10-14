
#include <iostream>

#include "console.h"
#include "timer.h"

int Console::rank;
int Console::verbosity;

void Console::init(int rank, int verbosity) {
    Console::rank = rank;
    Console::verbosity = verbosity;
}

void Console::log(int verbosity, const char* str) {
    if (verbosity > Console::verbosity)
        return;
    printf("[%3.3f] ", Timer::elapsedSeconds());
    std::cout << "[" << rank << "] " << str << std::endl;
}
void Console::log(int verbosity, std::string str) {
    log(verbosity, str.c_str());
}
void Console::log_send(int verbosity, std::string str, int destRank) {
    log(verbosity, str + " => [" + std::to_string(destRank) + "]");
}
void Console::log_recv(int verbosity, std::string str, int sourceRank) {
    log(verbosity, str + " <= [" + std::to_string(sourceRank) + "]");
}