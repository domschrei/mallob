
#include <iostream>

#include "console.h"
#include "timer.h"

int Console::rank;

void Console::init(int rank) {
    Console::rank = rank;
}

void Console::log(const char* str) {
    printf("[%3.3f] ", Timer::elapsedSeconds());
    std::cout << "[" << rank << "] " << str << std::endl;
}
void Console::log(std::string str) {
    log(str.c_str());
}
void Console::log_send(std::string str, int destRank) {
    log(str + " => [" + std::to_string(destRank) + "]");
}
void Console::log_recv(std::string str, int sourceRank) {
    log(str + " <= [" + std::to_string(sourceRank) + "]");
}