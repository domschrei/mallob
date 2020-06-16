
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <sys/wait.h>
#include <iostream>
#include <exception>
#include <execinfo.h>
#include <signal.h>

#include "fork.hpp"
#include "proc.hpp"
#include "util/console.hpp"
//#include "backtrace.hpp"

void propagateSignalAndExit(int signum) {

    std::set<int> children = Fork::_children;
    
    // Propagate signal to children
    for (pid_t child : children) {
        kill(child, SIGTERM);
        kill(child, SIGCONT);
    }

    /*
    // Hard kill all remaining processes after 1 second
    usleep(1000 * 1000);
    for (pid_t child : children) {
        kill(child, SIGKILL);
    }
    */

    // Exit yourself
    exit(signum == SIGABRT || signum == SIGSEGV ? 1 : 0);
}

void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleAbort(int sig) {
    void *array[20];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 20);

    // print out all the frames
    Console::log(Console::CRIT, "Error from pid=%ld tid=%ld: signal %d. Backtrace:\n", Proc::getPid(), Proc::getTid(), sig);
    char** bt = backtrace_symbols(array, size);
    for (int i = 0; i < size; i++) {
        Console::log(Console::CRIT, "- %s", bt[i]);
    }

    // Send exit signals to children and exit yourself
    propagateSignalAndExit(sig);
}


int Fork::_rank;
std::set<pid_t> Fork::_children;

void Fork::init(int rank) {

    /*
    struct sigaction sa;
    sa.sa_sigaction = (void*) bt_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    */

    signal(SIGSEGV, handleAbort);
    signal(SIGABRT, handleAbort);
    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGTERM, propagateSignalAndExit);
    signal(SIGINT, propagateSignalAndExit);

    _rank = rank;
    _children.clear();
}

pid_t Fork::createChild() {
    pid_t res = fork();
    if (res > 0) {
        // parent process
        _children.insert(res);
    } else if (res == 0) {
        // child process
        _children.clear();
    }
    return res;
}
void Fork::terminate(pid_t childpid) {
    kill(childpid, SIGTERM);
    kill(childpid, SIGCONT);
    _children.erase(childpid);
    //_pending_exiting_children++;
}
void Fork::hardkill(pid_t childpid) {
    kill(childpid, SIGKILL);
}
void Fork::suspend(pid_t childpid) {
    kill(childpid, SIGTSTP);
}
void Fork::resume(pid_t childpid) {
    kill(childpid, SIGCONT);
}
void Fork::wakeUp(pid_t childpid) {
    kill(childpid, SIGUSR1);
}
void Fork::terminateAll() {
    std::set<int> children = _children;
    for (int childpid : children) {
        terminate(childpid);
    }
}
bool Fork::didChildExit(pid_t childpid) {
    int status;
    pid_t result = waitpid(childpid, &status, WNOHANG /*| WUNTRACED | WCONTINUED*/);
    return result > 0;
}