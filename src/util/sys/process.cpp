
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <iostream>
#include <exception>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>

#include "util/assert.hpp"

#include "process.hpp"
#include "proc.hpp"
#include "util/logger.hpp"
#include "util/sys/stacktrace.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/thread_pool.hpp"

int Process::_rank;
std::string Process::_trace_dir;

Mutex Process::_children_mutex;
std::set<pid_t> Process::_children;
std::atomic_bool Process::_main_process;
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
    if (!Process::_exit_signal_caught) {
        Process::_exit_signal = signum;
        Process::_signal_tid = Proc::getTid();
        Process::_exit_signal_caught = true;
        if (Process::isCrash(signum)) {
            // Try to write a trace of the concerned thread with gdb
            Process::writeTrace(Process::_signal_tid);
        }
    }
    
    // Special handling for termination and crash signals
    Process::handleTerminationSignal(Process::getCaughtSignal().value());
}

bool Process::isCrash(int signum) {
    return signum == SIGABRT || signum == SIGFPE || signum == SIGSEGV || signum == SIGBUS;
}

void Process::handleTerminationSignal(const SignalInfo& info) {
    if (Process::isMainProcess()) Process::forwardTerminateToChildren();

    if (isCrash(info.signum)) {
        if (Proc::getTid() == Process::_main_tid) {
            // Main thread: handle crash directly
            long tid = info.tid;
            LOG(V0_CRIT, "[ERROR] pid=%ld tid=%ld signal=%d\n", 
                    Proc::getPid(), tid, info.signum);
            // Try to write a trace of the concerned thread with gdb
            Process::writeTrace(tid);
            Process::doExit(1);
        } else {
            // Sleep indefinitely until killed by main thread
            while (true) {}
        }
    }
}

void Process::doExit(int retval) {
    // Exit with normal exit code if terminated or interrupted,
    // with caught signal otherwise
    exit((retval == SIGTERM || retval == SIGINT) ? 0 : retval);
}


void Process::init(int rank, const std::string& traceDir, bool leafProcess) {
    _rank = rank;
    _trace_dir = traceDir;
    _exit_signal_caught = false;
    _main_process = !leafProcess;
    _main_tid = Proc::getTid();

    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGSEGV, handleSignal);
    signal(SIGABRT, handleSignal);
    signal(SIGFPE, handleSignal);
    signal(SIGBUS, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGINT,  handleSignal);

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
    sendSignal(childpid, SIGTSTP);
}
void Process::resume(pid_t childpid) {
    sendSignal(childpid, SIGCONT);
}
void Process::wakeUp(pid_t childpid) {
    sendSignal(childpid, SIGUSR1);
}

bool Process::isMainProcess() {
    return _main_process;
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
    int result = kill(childpid, signum);
    if (result == -1) {
        LOG(V1_WARN, "[WARN] kill -%i %i returned -1\n", signum, childpid);
    }
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
            + " && gdb --q --n --ex bt --batch --pid " + std::to_string(tid) 
            + " > " + _trace_dir + "/mallob_thread_trace_of_" + std::to_string(tid) 
            + "_from_" + std::to_string(callingTid) + " 2>&1"
            + " && kill -18 " + std::to_string(tid);
    // Execute GDB in separate thread to avoid self-tracing
    std::thread thread([&]() {
        system(command.c_str());
    });
    // Wait for completion
    thread.join();
}
