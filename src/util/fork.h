
#ifndef DOMPASCH_MALLOB_FORK_H
#define DOMPASCH_MALLOB_FORK_H

#include <set>

class Fork {

public:
    static int _rank;
    static std::set<int> _children;

    static void init(int rank);
    static int createChild();
    static void terminate(int childpid);
    static void hardkill(int childpid);
    static void suspend(int childpid);
    static void resume(int childpid);
    static void wakeUp(int childpid);
    static void terminateAll();

    /* 0: running, -1: error, childpid: exited */
    static bool didChildExit(int childpid);

};

#endif