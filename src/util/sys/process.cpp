
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <sys/wait.h>
#include <iostream>
#include <exception>
#include <execinfo.h>
#include <signal.h>
#include <assert.h>

#include "process.hpp"
#include "proc.hpp"
#include "util/logger.hpp"
#include "util/sys/stacktrace.hpp"


int Process::_rank;
std::set<pid_t> Process::_children;
std::atomic_bool Process::_modifying_children;
std::atomic_bool Process::_exit_signal_caught = false;
std::atomic_int Process::_exit_signal = 0;



void propagateSignalAndExit(int signum) {
    Process::forwardTerminateToChildren();
    Process::_exit_signal_caught = true;
    Process::_exit_signal = signum;
}

void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleAbort(int sig) {
    
    // print out all the frames
    log(V0_CRIT, "Error from pid=%ld tid=%ld signal=%d\n", Proc::getPid(), Proc::getTid(), sig);
    log(V0_CRIT, "Backtrace: \n%s\n", backtrace().c_str());

    // additionally write a trace of this thread found by gdb
    Process::writeTrace(Proc::getTid());

    // Send exit signals to children and exit yourself
    Process::forwardTerminateToChildren();
    exit(sig);
}



void Process::init(int rank, bool leafProcess) {

    _rank = rank;
    _modifying_children = false;
    _exit_signal_caught = false;

    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGSEGV, handleAbort);
    signal(SIGABRT, handleAbort);

    if (!leafProcess) {
        signal(SIGTERM, propagateSignalAndExit);
        signal(SIGINT, propagateSignalAndExit);
    }
}

pid_t Process::createChild() {
    pid_t res = fork();

    _modifying_children = true;
    if (res > 0) {
        // parent process
        _children.insert(res);
    } else {
        assert(res >= 0);
    }
    _modifying_children = false;

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
void Process::terminateAll() {
    std::set<int> children = _children;
    for (int childpid : children) {
        terminate(childpid);
        resume(childpid);
    }
}

void Process::forwardTerminateToChildren() {
    if (!Process::_modifying_children) {
        // Propagate signal to children
        for (pid_t child : Process::_children) {
            Process::sendSignal(child, SIGTERM);
            Process::sendSignal(child, SIGCONT);
        }
    }
}

void Process::sendSignal(pid_t childpid, int signum) {
    int result = kill(childpid, signum);
    if (result == -1) {
        log(V1_WARN, "[WARN] kill -%i %i returned -1\n", signum, childpid);
    }
}

bool Process::didChildExit(pid_t childpid) {

    if (!_children.count(childpid)) return true;
    
    int status;
    pid_t result = waitpid(childpid, &status, WNOHANG /*| WUNTRACED | WCONTINUED*/);
    if (result != 0) {
        _modifying_children = true;
        _children.erase(childpid);
        _modifying_children = false;
        return true;
    }
    return false;
}

std::optional<int> Process::isExitSignalCaught() {
    std::optional<int> opt;
    if (_exit_signal_caught) opt = _exit_signal;
    return opt;
}

void Process::writeTrace(long tid) {
    long callingTid = Proc::getTid();
    std::string command = "gdb --q --n --ex bt --batch --pid " + std::to_string(tid) 
            + " > mallob_thread_trace_" + std::to_string(tid) 
            + "_by_" + std::to_string(callingTid) + " 2>&1";
    system(command.c_str());
}
