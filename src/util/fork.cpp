
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "fork.h"


void propagateSignal(int signum) {
    for (pid_t child : Fork::_children) {
        kill(child, signum);
    }
}


std::set<pid_t> Fork::_children;

void Fork::init() {
    signal(SIGTERM, propagateSignal);
    signal(SIGINT, propagateSignal);
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
    _children.erase(childpid);
}
void Fork::suspend(pid_t childpid) {
    kill(childpid, SIGSTOP);
}
void Fork::resume(pid_t childpid) {
    kill(childpid, SIGCONT);
}