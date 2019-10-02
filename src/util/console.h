
#ifndef DOMPASCH_CONSOLE_H
#define DOMPASCH_CONSOLE_H

#include <string>

class Console {

private:
    static int rank;

public:
    static void init(int rank);
    static void log(const char* str);
    static void log(std::string str);
    static void log_send(std::string str, int destRank);
    static void log_recv(std::string str, int sourceRank);

};

#endif