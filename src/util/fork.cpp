
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <sys/wait.h>

#include "fork.h"


void propagateSignalAndExit(int signum) {
    for (pid_t child : Fork::_children) {
        kill(child, signum);
    }
    exit(0);
}

void acknowledgeChildExit(int signum) {
    Fork::_pending_exiting_children--;
}


std::set<pid_t> Fork::_children;
int Fork::_pending_exiting_children = 0;

void Fork::init() {
    signal(SIGTERM, propagateSignalAndExit);
    signal(SIGINT, propagateSignalAndExit);
    signal(SIGCHLD, acknowledgeChildExit);
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
    kill(childpid, SIGCONT);
    kill(childpid, SIGTERM);
    _children.erase(childpid);
    _pending_exiting_children++;
}
void Fork::terminateAll() {
    std::set<int> children = _children;
    for (int childpid : children) {
        terminate(childpid);
    }
}
void Fork::suspend(pid_t childpid) {
    kill(childpid, SIGSTOP);
}
void Fork::resume(pid_t childpid) {
    kill(childpid, SIGCONT);
}
int Fork::getChildStatus(pid_t childpid) {
    int status;
    pid_t result = waitpid(childpid, &status, WNOHANG | WUNTRACED | WCONTINUED);
    return result;
}
bool Fork::allChildrenSignalsArrived() {
    return _pending_exiting_children == 0;
}