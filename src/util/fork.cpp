
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <sys/wait.h>

#include "fork.h"


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
    exit(0);
}

void doNothing(int signum) {
    // Do nothing, just return
}


int Fork::_rank;
std::set<pid_t> Fork::_children;

void Fork::init(int rank) {
    _rank = rank;
    _children.clear();
    signal(SIGTERM, propagateSignalAndExit);
    signal(SIGINT, propagateSignalAndExit);
    signal(SIGUSR1, doNothing);
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