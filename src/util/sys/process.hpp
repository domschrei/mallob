
#ifndef DOMPASCH_MALLOB_FORK_H
#define DOMPASCH_MALLOB_FORK_H

#include <set>
#include <atomic>
#include <optional>
#include <thread>

#include "util/sys/threading.hpp"

class Process {

public:
    static int _rank;

    static Mutex _children_mutex;
    static std::set<pid_t> _children;
    static std::atomic_bool _main_process;

    static std::atomic_bool _exit_signal_caught;
    static std::atomic_int _exit_signal;

    static std::thread _terminate_checker;

    static void init(int rank, bool leafProcess = false);
    
    static int createChild();
    
    static void terminate(pid_t childpid);
    static void hardkill(pid_t childpid);
    static void suspend(pid_t childpid);
    static void resume(pid_t childpid);
    static void wakeUp(pid_t childpid);

    static void sendSignal(pid_t childpid, int signum);

    static void forwardTerminateToChildren();

    /* 0: running, -1: error, childpid: exited */
    static bool didChildExit(pid_t childpid);
    static std::optional<int> isExitSignalCaught();

    static void writeTrace(long tid);

    static void doExit(int retval);
};

#endif