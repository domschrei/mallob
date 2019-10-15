
#include <iostream>
#include <ostream>

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

void Console::init(int rank, int verbosity, bool coloredOutput) {
    Console::rank = rank;
    Console::verbosity = verbosity;
    Console::coloredOutput = coloredOutput;
}

void Console::log(int verbosity, const char* str) {
    if (verbosity > Console::verbosity)
        return;

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

    printf("[%3.3f] ", Timer::elapsedSeconds());
    std::cout << "[" << rank << "] " << str ;
    
    if (coloredOutput) {
        std::cout << Modifier(Code::FG_DEFAULT);
    }

    std::cout << std::endl;
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