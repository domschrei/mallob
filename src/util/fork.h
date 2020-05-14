
#ifndef DOMPASCH_MALLOB_FORK_H
#define DOMPASCH_MALLOB_FORK_H

#include <set>

class Fork {

public:
    static std::set<int> _children;
    static int _pending_exiting_children;

    static void init();
    static int createChild();
    static void terminate(int childpid);
    static void suspend(int childpid);
    static void resume(int childpid);

    /* 0: running, -1: error, childpid: exited */
    static int getChildStatus(int childpid);
    static bool allChildrenSignalsArrived();

};

#endif