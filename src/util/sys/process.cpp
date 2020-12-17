
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
#include "util/console.hpp"
#include "util/sys/stacktrace.hpp"


int Process::_rank;
std::set<pid_t> Process::_children;
bool Process::_modifying_children;
bool Process::_exit_signal_caught;



void propagateSignalAndExit(int signum) {

    if (!Process::_modifying_children) {
        // Propagate signal to children
        for (pid_t child : Process::_children) {
            Process::sendSignal(child, SIGTERM);
            Process::sendSignal(child, SIGCONT);
        }
    }

    // Exit yourself
    Process::_exit_signal_caught = true;
}

void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleAbort(int sig) {
    
    // print out all the frames
    Console::log(Console::CRIT, "Error from pid=%ld tid=%ld signal=%d", Proc::getPid(), Proc::getTid(), sig);
    Console::log(Console::CRIT, "Backtrace: \n%s", backtrace().c_str());

    // Send exit signals to children and exit yourself
    propagateSignalAndExit(sig);
}



void Process::init(int rank, bool leafProcess) {

    /*
    struct sigaction sa;
    sa.sa_sigaction = (void*) bt_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    */

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

void Process::sendSignal(pid_t childpid, int signum) {
    int result = kill(childpid, signum);
    if (result == -1) {
        Console::log(Console::WARN, "[WARN] kill -%i %i returned -1", signum, childpid);
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

bool Process::isExitSignalCaught() {
    return _exit_signal_caught;
}
