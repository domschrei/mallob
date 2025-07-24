
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <errno.h>
#include <cstdlib>
#include <thread>

#include "process.hpp"
#include "proc.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/assert.hpp"

int Process::_rank;
std::string Process::_trace_dir;

Mutex Process::_children_mutex;
std::set<pid_t> Process::_children;
long Process::_main_tid;

std::atomic_bool Process::_exit_signal_caught = false;
std::atomic_int Process::_exit_signal = 0;
std::atomic_long Process::_signal_tid = 0;

void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleSignal(int signum) {
    // Do not recursively catch signals (if something goes wrong in here)
    if (Process::_exit_signal_caught) return;

    Process::_exit_signal_caught = true;
    Process::_exit_signal = signum;
    Process::_signal_tid = Proc::getTid();

    // If crash, try to write a trace of the concerned thread with gdb
    if (Process::isCrash(signum)) Process::writeTrace(Proc::getTid());

    // Impose a hard timeout for this process' lifetime from this point,
    // to avoid indeterminate freezes
    //auto cmd = "bash scripts/kill-delayed.sh " + std::to_string(Proc::getPid());
    //system(cmd.c_str());

    // Special case where we are in the main thread and a crash was noticed:
    // normal execution cannot continue here, so we exit directly.
    if (Process::isCrash(signum) && Process::_main_tid == Proc::getTid()) {
        Process::reportTerminationSignal(Process::getCaughtSignal().value());
        Process::forwardTerminateToChildren();
        Process::doExit(signum);
    }
}

bool Process::isCrash(int signum) {
    return signum == SIGABRT || signum == SIGFPE || signum == SIGSEGV || signum == SIGBUS || signum == SIGILL;
}

void Process::reportTerminationSignal(const SignalInfo& info) {
    assert(Proc::getTid() == Process::_main_tid);

    if (isCrash(info.signum)) {
        LOG(V0_CRIT, "[ERROR] pid=%ld tid=%ld signal=%d\n", 
                Proc::getPid(), info.tid, info.signum);
    } else {
        LOG(V3_VERB, "pid=%ld tid=%ld signal=%d\n",
                Proc::getPid(), info.tid, info.signum);
    }
    Logger::getMainInstance().flush();
}

void Process::removeDelayedExitWatchers() {
    //std::string cmd = "pgrep -f \"do-kill-delayed.sh " + std::to_string(Proc::getPid()) + "\" | xargs kill >/dev/null 2>&1";
    //system(cmd.c_str());
}

void Process::doExit(int retval) {
    // Exit with normal exit code if terminated or interrupted,
    // with caught signal otherwise
    exit((retval == SIGTERM || retval == SIGINT) ? 0 : retval);
}


void Process::init(int rank, const std::string& traceDir) {
    _rank = rank;
    _trace_dir = traceDir;
    _exit_signal_caught = false;
    _main_tid = Proc::getTid();

    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGSEGV, handleSignal);
    signal(SIGABRT, handleSignal);
    signal(SIGFPE, handleSignal);
    signal(SIGBUS, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGINT,  handleSignal);
    signal(SIGPIPE, SIG_IGN);

    // Allow any other process/thread to ptrace this process
    // (used for debugging of crashes via external gdb call)
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
}

pid_t Process::createChild() {
    pid_t res = fork();

    if (res > 0) {
        // parent process
        auto lock = _children_mutex.getLock();
        _children.insert(res);
    } else if (res == -1) {
        // fork failed
        LOG(V0_CRIT, "[ERROR] fork failed, errno %i\n", (int)errno);
        abort();
    }

    return res;
}

void Process::terminate(pid_t childpid) {
    sendSignal(childpid, SIGTERM);
}
void Process::hardkill(pid_t childpid) {
    sendSignal(childpid, SIGKILL);
}
void Process::suspend(pid_t childpid) {
    sendSignal(childpid, SIGSTOP);
}
void Process::resume(pid_t childpid) {
    sendSignal(childpid, SIGCONT);
}
void Process::wakeUp(pid_t childpid) {
    sendSignal(childpid, SIGUSR1);
}

void Process::forwardTerminateToChildren() { 
    // Propagate signal to children
    auto lock = _children_mutex.getLock();
    for (pid_t childpid : Process::_children) {
        terminate(childpid);
        resume(childpid);
    }
}

void Process::sendSignal(pid_t childpid, int signum) {
    assert(childpid > 0);
    int result = kill(childpid, signum);
    if (result == -1) {
        LOG(V1_WARN, "[WARN] kill -%i %i returned errno %i\n", signum, childpid, errno);
    }
}

pthread_t Process::getPthreadId() {
    return pthread_self();
}
void Process::wakeUpThread(pthread_t pthreadId) {
    sendPthreadSignal(pthreadId, SIGUSR1);
}
void Process::sendPthreadSignal(pthread_t pthreadId, int sig) {
    pthread_kill(pthreadId, sig);
}

bool Process::didChildExit(pid_t childpid, int* exitStatusOut) {
    auto lock = _children_mutex.getLock();

    if (!_children.count(childpid)) {
        if (exitStatusOut != nullptr) 
            *exitStatusOut = 0;
        return true;
    }
    
    int status;
    pid_t result = waitpid(childpid, &status, WNOHANG /*| WUNTRACED | WCONTINUED*/);
    if (result != 0) {
        _children.erase(childpid);
        if (exitStatusOut != nullptr) 
            *exitStatusOut = status;
        return true;
    }
    return false;
}

std::optional<Process::SignalInfo> Process::getCaughtSignal() {
    std::optional<SignalInfo> opt;
    if (_exit_signal_caught) {
        opt = SignalInfo{_exit_signal, Proc::getPid(), _signal_tid};
    }
    return opt;
}

void Process::writeTrace(long tid) {
    long callingTid = Proc::getTid();
    std::string command = "kill -20 " + std::to_string(tid)
            + " ; gdb --q --n --ex bt --batch --pid " + std::to_string(tid)
            + " > " + _trace_dir + "/mallob_thread_trace_of_" + std::to_string(tid)
            + "_from_" + std::to_string(callingTid) + " 2>&1"
            + " ; kill -18 " + std::to_string(tid);
    // Execute GDB in separate thread to avoid self-tracing
    std::thread thread([&]() {
        (void) system(command.c_str());
    });
    // Wait for completion
    thread.join();
}
