
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

#include "fork.hpp"
#include "proc.hpp"
#include "util/console.hpp"

void propagateSignalAndExit(int signum) {

    if (!Fork::_modifying_children) {
        
        // Propagate signal to children
        for (pid_t child : Fork::_children) {
            Fork::sendSignal(child, SIGTERM);
            Fork::sendSignal(child, SIGCONT);
        }
    }

    // Exit yourself
    exit(signum == SIGABRT || signum == SIGSEGV ? 1 : 0);
}

void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleAbort(int sig) {
    const int maxArraySize = 30;
    void *array[maxArraySize];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, maxArraySize);

    // print out all the frames
    Console::log(Console::CRIT, "Error from pid=%ld tid=%ld signal=%d - Backtrace:\n", Proc::getPid(), Proc::getTid(), sig);
    char** bt = backtrace_symbols(array, size);
    for (size_t i = 0; i < size; i++) {
        Console::log(Console::CRIT, "- %s", bt[i]);
    }

    // Send exit signals to children and exit yourself
    propagateSignalAndExit(sig);
}

int Fork::_rank;
std::set<pid_t> Fork::_children;
bool Fork::_modifying_children;

void Fork::init(int rank, bool leafProcess) {

    /*
    struct sigaction sa;
    sa.sa_sigaction = (void*) bt_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    */

    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGSEGV, handleAbort);
    signal(SIGABRT, handleAbort);

    if (!leafProcess) {
        signal(SIGTERM, propagateSignalAndExit);
        signal(SIGINT, propagateSignalAndExit);
    }

    _rank = rank;
    _modifying_children = false;

    setenv("PATH", ("build/app/sat:" + std::string((const char*) getenv("PATH"))).c_str(), /*overwrite=*/1);
}

pid_t Fork::createChild() {
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

void Fork::terminate(pid_t childpid) {
    sendSignal(childpid, SIGTERM);
}
void Fork::hardkill(pid_t childpid) {
    sendSignal(childpid, SIGKILL);
}
void Fork::suspend(pid_t childpid) {
    sendSignal(childpid, SIGTSTP);
}
void Fork::resume(pid_t childpid) {
    sendSignal(childpid, SIGCONT);
}
void Fork::wakeUp(pid_t childpid) {
    sendSignal(childpid, SIGUSR1);
}
void Fork::terminateAll() {
    std::set<int> children = _children;
    for (int childpid : children) {
        terminate(childpid);
        resume(childpid);
    }
}

void Fork::sendSignal(pid_t childpid, int signum) {
    int result = kill(childpid, signum);
    if (result == -1) {
        Console::log(Console::WARN, "[WARN] kill -%i %i returned -1", signum, childpid);
    }
}

bool Fork::didChildExit(pid_t childpid) {

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