
#ifndef DOMPASCH_MALLOB_FORK_H
#define DOMPASCH_MALLOB_FORK_H

#include <pthread.h>
#include <sys/types.h>
#include <set>
#include <atomic>
#include <optional>
#include <string>

#include "util/sys/threading.hpp"

class BackgroundWorker; // forward declaration
class Mutex;

class Process {

public:
    static int _rank;
    static std::string _trace_dir;

    static Mutex _children_mutex;
    static std::set<pid_t> _children;
    static long _main_tid;

    static volatile bool _exit_signal_caught;
    static std::atomic_int _exit_signal;
    static std::atomic_long _signal_tid;
    static std::atomic_bool _exit_signal_digested;

    static void init(int rank, const std::string& traceDir = ".");
    
    static int createChild();
    
    static void terminate(pid_t childpid);
    static void hardkill(pid_t childpid);
    static void suspend(pid_t childpid);
    static void resume(pid_t childpid);
    static void wakeUp(pid_t childpid);

    static void sendSignal(pid_t childpid, int signum);

    static pthread_t getPthreadId();
    static void wakeUpThread(pthread_t pthreadId);
    static void sendPthreadSignal(pthread_t pthreadId, int sig);

    static void forwardTerminateToChildren();

    /* 0: running, -1: error, childpid: exited */
    static bool didChildExit(pid_t childpid, int* exitStatusOut = nullptr);

    static inline bool wasSignalCaught() {return _exit_signal_caught;}
    struct SignalInfo {
        int signum;
        pid_t pid;
        long tid;
    };
    static std::optional<SignalInfo> getCaughtSignal();
    static bool isCrash(int signum);
    static void reportTerminationSignal(const SignalInfo& info);

    static void writeTrace(long tid);

    static void removeDelayedExitWatchers();

    static void doExit(int retval);
};

#endif