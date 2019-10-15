
#ifndef DOMPASCH_CONSOLE_H
#define DOMPASCH_CONSOLE_H

#include <string>

class Console {

public:
    static const int CRIT = 0;
    static const int WARN = 1;
    static const int INFO = 2;
    static const int VERB = 3;
    static const int VVERB = 4;
    static const int VVVERB = 5;

private:
    static int rank;
    static int verbosity;
    static bool coloredOutput;
public:
    static void init(int rank, int verbosity, bool coloredOutput);
    static void log(int verbosity, const char* str);
    static void log(int verbosity, std::string str);
    static void log_send(int verbosity, std::string str, int destRank);
    static void log_recv(int verbosity, std::string str, int sourceRank);

};

#endif