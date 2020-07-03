
#ifndef DOMPASCH_MALLOB_FORK_H
#define DOMPASCH_MALLOB_FORK_H

#include <set>

#include "util/sys/threading.hpp"

class Fork {

public:
    static int _rank;
    static std::set<pid_t> _children;
    static bool _modifying_children;

    static void init(int rank, bool leafProcess = false);
    
    static int createChild();
    
    static void terminate(pid_t childpid);
    static void hardkill(pid_t childpid);
    static void suspend(pid_t childpid);
    static void resume(pid_t childpid);
    static void wakeUp(pid_t childpid);

    static void terminateAll();

    static void sendSignal(pid_t childpid, int signum);

    /* 0: running, -1: error, childpid: exited */
    static bool didChildExit(pid_t childpid);
};

#endif